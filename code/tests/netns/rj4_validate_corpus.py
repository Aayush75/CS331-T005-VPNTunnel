#!/usr/bin/env python3
"""A configuration the controller accepts must be accepted by the kernel's VALIDATE.

Two implementations check the same rules: the controller's configuration loader and the kernel's
snapshot validation. A configuration the controller accepts but the kernel refuses would only show up
at run time as a controller that cannot start, so this test searches for one.

A seeded generator builds configurations that are valid by construction, then applies up to three
deliberate mutations (a boundary value, an overlap, a reserved range, a duplicate, a limit exceeded).
Every configuration goes through `vptctl snapshot`, which is the controller's own loader and snapshot
encoder, and every one it accepts is sent to the real kernel with VALIDATE. The property: the kernel
accepts all of them. Configurations the controller rejects are counted by reason, to show the generator
reaches the rules it is meant to probe.

Two controls keep the property from passing vacuously: a hand-made snapshot that breaks one kernel
rule must be refused (VALIDATE can say no), and the checked-in sample must be accepted by both.
The random corpus cannot notice a controller that is stricter than the kernel, so a short fixed list
of hand-written configurations at the limits (must_accept) must be accepted by both. The same list is
checked in as TOML under control/tests/vectors/must_accept/ so the controller half also runs in
`cargo test`; `--write-must-accept` regenerates those files and the run fails if they drift.
A last, smaller part feeds corrupted snapshots to VALIDATE: any verdict is fine, a hang or a kernel
fault is not.

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl rj4_validate_corpus.py [--count N] [--seed S] [--keep DIR]
"""
import argparse
import collections
import ipaddress
import os
import random
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_genl as G  # noqa: E402  (raw Generic Netlink helpers)

REPO = os.path.dirname(os.path.dirname(HERE))
passed = failed = 0
LAN_IF, UND_IF = 11, 12  # interface indexes encoded into snapshots; VALIDATE only needs them nonzero


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


# ---- Generator ----------------------------------------------------------------------------------

def net(rng, lo_len, hi_len, first_octets, avoid=()):
    """A random canonical IPv4 network not intersecting any of `avoid`, or None."""
    for _ in range(60):
        length = rng.randint(lo_len, hi_len)
        addr = rng.choice(first_octets) << 24 | rng.getrandbits(24)
        n = ipaddress.ip_network((addr, length), strict=False)
        if not any(n.overlaps(x) for x in avoid):
            return n
    return None


PUBLIC_FIRST = [o for o in range(1, 224) if o not in (10, 127, 169, 172, 192)]
LOCAL_POOLS = [[10], [192], [172]]


def host_in(rng, n, exclude=()):
    hosts = list(n.hosts()) if n.num_addresses <= 64 else None
    for _ in range(80):
        h = (hosts and rng.choice(hosts)) or ipaddress.ip_address(
            int(n.network_address) + rng.randint(1, n.num_addresses - 2))
        if h not in exclude and h != n.network_address and h != n.broadcast_address:
            return h
    return None


def sub(rng, parent, max_len=32):
    length = rng.randint(parent.prefixlen, max_len)
    off = rng.getrandbits(max(0, length - parent.prefixlen)) if length > parent.prefixlen else 0
    return ipaddress.ip_network(
        (int(parent.network_address) + (off << (32 - length)), length), strict=False)


def build(rng, keys, local_pub):
    """A configuration model that both implementations should accept."""
    lan = net(rng, 8, 28, rng.choice(LOCAL_POOLS))
    und = net(rng, 16, 28, [172, 198, 100], avoid=[lan])
    peers = []
    slots = rng.sample([1, 2, 3, 4], rng.choice([1, 1, 2, 2, 3, 4]))
    taken = [lan, und]
    used_hosts = set()
    ends = set()
    own = host_in(rng, und)
    for i, slot in enumerate(sorted(slots, key=lambda _: rng.random())):
        prefixes = []
        for _ in range(rng.choice([1, 1, 2, 3, 4])):
            p = net(rng, 8, 32, PUBLIC_FIRST, avoid=taken)
            if p:
                prefixes.append(p)
                taken.append(p)
        if not prefixes:
            continue
        ep = host_in(rng, und, exclude={own} | ends)
        ends.add(ep)
        peers.append(dict(
            name=f"peer{i}", slot=slot, key=keys[i], endpoint=str(ep),
            role=rng.choice(["initiator", "responder"]),
            path_mtu=rng.choice([None, 1280, 1500, rng.randint(1280, 1500)]),
            prefixes=[str(p) for p in prefixes]))
    rules, ids = [], set()
    for _ in range(rng.choice([1, 1, 2, 3, 5, 12, 40, 128])):
        peer = rng.choice(peers)
        pfx = ipaddress.ip_network(rng.choice(peer["prefixes"]))
        local_side, peer_side = sub(rng, lan), sub(rng, pfx)
        outbound = rng.random() < 0.5
        proto = rng.choice(["tcp", "udp", "icmp-echo"])
        rid = rng.choice([rng.randint(1, 500), rng.randint(1, 2 ** 32 - 1)])
        while rid in ids:
            rid = rng.randint(1, 2 ** 32 - 1)
        ids.add(rid)
        rules.append(dict(
            id=rid, peer=peer["name"],
            source=str(local_side if outbound else peer_side),
            destination=str(peer_side if outbound else local_side),
            protocol=proto,
            ports=None if proto == "icmp-echo" else rng.choice(
                ["80", "22", "65535", "1", "1000-2000", "1-65535", "443-443"])))
    return dict(
        gw=dict(name="gw", lan_interface="lan0", underlay_interface="out0",
                lan_address=f"{host_in(rng, lan)}/{lan.prefixlen}",
                underlay_address=f"{own}/{und.prefixlen}"),
        peers=peers, rules=rules, lan=lan, und=und, local_pub=local_pub)


def mutations(rng, m, keys):
    """Each mutation moves one thing to or past a boundary. Most make the configuration invalid."""
    P = lambda: rng.choice(m["peers"])  # noqa: E731
    R = lambda: rng.choice(m["rules"]) if m["rules"] else None  # noqa: E731

    def mtu():
        P()["path_mtu"] = rng.choice([1279, 1281, 1499, 1501, 0, 65535, 1280, 1500])

    def prefix_reserved():
        P()["prefixes"].append(rng.choice(
            ["0.0.0.0/0", "0.0.0.0/8", "127.0.0.0/8", "127.1.2.0/24", "169.254.0.0/16", "224.0.0.0/4",
             "239.1.1.0/24", "240.0.0.0/4", "255.255.255.255/32", "1.2.3.4/32", "10.0.0.0/8"]))

    def prefix_overlap():
        p = P()
        p["prefixes"].append(str(rng.choice([m["lan"], m["und"],
                                             sub(rng, m["lan"]), sub(rng, m["und"]),
                                             ipaddress.ip_network(rng.choice(P()["prefixes"]))])))

    def prefix_noncanonical():
        p = P()
        n = ipaddress.ip_network(p["prefixes"][0])
        if n.prefixlen < 32:
            p["prefixes"][0] = f"{n.network_address + 1}/{n.prefixlen}"

    def prefix_count():
        p = P()
        p["prefixes"] = rng.choice([[], p["prefixes"] * 2, p["prefixes"][:1] * 5,
                                    p["prefixes"] + [f"{rng.randint(1, 120)}.9.9.0/24" for _ in range(4)]])

    def endpoint():
        p = P()
        p["endpoint"] = str(rng.choice([
            m["und"].network_address, m["und"].broadcast_address, m["gw"]["underlay_address"].split("/")[0],
            "8.8.8.8", "0.0.0.0", "255.255.255.255", "127.0.0.1",
            ipaddress.ip_network(P()["prefixes"][0]).network_address + 1,
            P()["endpoint"]]))

    def slot():
        P()["slot"] = rng.choice([0, 5, 255, P()["slot"]])

    def key():
        p = P()
        p["key"] = rng.choice([m["local_pub"], P()["key"], "00" * 32, "ff" * 32, "zz" * 32, "ab" * 31])

    def rule_count():
        extra = rng.choice([0, 128 - len(m["rules"]), 129 - len(m["rules"]), 200 - len(m["rules"])])
        if extra < 0:
            m["rules"] = m["rules"][:max(0, len(m["rules"]) + extra)]
            return
        r = m["rules"][0] if m["rules"] else None
        if r is None:
            return
        used = {x["id"] for x in m["rules"]}
        nxt = 1
        for k in range(extra):
            while nxt in used:
                nxt += 1
            used.add(nxt)
            # A different port each time: the controller refuses a rule that repeats another's match.
            m["rules"].append(dict(r, id=nxt, protocol="tcp", ports=str(2000 + k)))
        if not extra:
            m["rules"] = []

    def rule_id():
        r = R()
        if r:
            r["id"] = rng.choice([0, 1, R()["id"], 2 ** 32 - 1, 2 ** 32, -1])

    def rule_shape():
        r = R()
        if r:
            r["peer"] = rng.choice(["nobody", "", r["peer"]])
            r["source"], r["destination"] = rng.choice([
                (r["source"], r["source"]), ("0.0.0.0/0", r["destination"]), (r["source"], "0.0.0.0/0"),
                (r["destination"], r["source"]), ("10.1.1.1/32", "10.1.1.2/32"),
                (r["source"], str(m["und"])), (r["source"], r["destination"])])

    def rule_ports():
        r = R()
        if r:
            r["protocol"] = rng.choice(["tcp", "udp", "icmp-echo", "sctp"])
            r["ports"] = rng.choice([None, "0", "65536", "0-0", "2000-1000", "1-65535", "80,443", "-1",
                                     "80", "1000-2000", "", "abc"])

    def lan():
        g = m["gw"]
        g["lan_address"] = rng.choice([f"{ipaddress.ip_address(g['lan_address'].split('/')[0])}/{n}"
                                       for n in (0, 8, 24, 30, 31, 32)])

    def underlay():
        g = m["gw"]
        g["underlay_address"] = rng.choice([f"{g['underlay_address'].split('/')[0]}/{n}" for n in (0, 16, 24, 31, 32)] +
                                           [g["lan_address"], "127.0.0.1/8", "0.0.0.0/0"])

    def ifnames():
        g = m["gw"]
        g[rng.choice(["lan_interface", "underlay_interface"])] = rng.choice(
            ["lo", "vpn0", ".", "..", "a" * 15, "a" * 16, "a b", "eth0/1", "", "out0", "lan0"])

    def names():
        p = P()
        p["name"] = rng.choice(["", "x" * 80, P()["name"], "has space", "ok-name_1.2"])

    return [mtu, prefix_reserved, prefix_overlap, prefix_noncanonical, prefix_count, endpoint, slot, key,
            rule_count, rule_id, rule_shape, rule_ports, lan, underlay, ifnames, names]


def render(m, key_file):
    def q(s):
        return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'

    g = m["gw"]
    out = ["[gateway]", f"name = {q(g['name'])}", f"private_key_file = {q(key_file)}",
           f"lan_interface = {q(g['lan_interface'])}", f"lan_address = {q(g['lan_address'])}",
           f"underlay_interface = {q(g['underlay_interface'])}",
           f"underlay_address = {q(g['underlay_address'])}", ""]
    for p in m["peers"]:
        out += ["[[peer]]", f"name = {q(p['name'])}", f"slot = {p['slot']}", f"public_key = {q(p['key'])}",
                f"endpoint = {q(p['endpoint'])}", f"role = {q(p['role'])}"]
        if p["path_mtu"] is not None:
            out.append(f"path_mtu = {p['path_mtu']}")
        out += ["prefixes = [" + ", ".join(q(x) for x in p["prefixes"]) + "]", ""]
    for r in m["rules"]:
        out += ["[[rule]]", f"id = {r['id']}", f"peer = {q(r['peer'])}", f"source = {q(r['source'])}",
                f"destination = {q(r['destination'])}", f"protocol = {q(r['protocol'])}"]
        if r["ports"] is not None:
            out.append(f"ports = {q(r['ports'])}")
        out.append("")
    return "\n".join(out)


# ---- Fixed configurations both implementations must accept ---------------------------------------

FIXED_KEYS = [
    "ad8cbe47dec1eb6f5491880eb2612ef18d42b3c667c7b165c4047437fb86861f",
    "5b2e9c1d7a4f3068e1d2c3b4a5968778695a4b3c2d1e0f9a8b7c6d5e4f302112",
    "0c1d2e3f405162738495a6b7c8d9eaf0112233445566778899aabbccddeeff01",
    "7e6d5c4b3a291807f6e5d4c3b2a1908f7e6d5c4b3a291807f6e5d4c3b2a19083",
]
MUST_ACCEPT_KEY = "@KEY@"  # stands for the gateway's private key file in the checked-in fixtures
MUST_ACCEPT_DIR = os.path.join(REPO, "control/tests/vectors/must_accept")


def fixed(peers, rules, lan="10.77.1.1/24", und="172.31.250.11/24"):
    """A hand-written configuration model in the shape `render` takes."""
    return dict(gw=dict(name="gw", lan_interface="lan0", underlay_interface="out0",
                        lan_address=lan, underlay_address=und), peers=peers, rules=rules)


def fpeer(i, prefixes, pmtu=1500, role="initiator"):
    return dict(name=f"peer{i}", slot=i + 1, key=FIXED_KEYS[i], endpoint=f"172.31.250.{12 + i}",
                role=role, path_mtu=pmtu, prefixes=prefixes)


def frule(i, peer="peer0", src="10.77.1.10/32", dst="10.77.2.10/32", proto="tcp", ports="8080"):
    return dict(id=i, peer=peer, source=src, destination=dst, protocol=proto,
                ports=None if proto == "icmp-echo" else ports)


def must_accept():
    one = [frule(1)]
    many = [frule(i, src=f"10.77.1.{i % 250 + 1}/32", ports=str(1000 + i)) for i in range(1, 129)]
    four = [f"10.77.{2 + k}.0/24" for k in range(4)]
    return {
        "min-path-mtu-1280": fixed([fpeer(0, ["10.77.2.0/24"], pmtu=1280)], one),
        "max-path-mtu-1500": fixed([fpeer(0, ["10.77.2.0/24"], pmtu=1500)], one),
        "default-path-mtu": fixed([fpeer(0, ["10.77.2.0/24"], pmtu=None)], one),
        "one-prefix-per-peer": fixed([fpeer(0, ["10.77.2.0/24"])], one),
        "four-prefixes-per-peer": fixed([fpeer(0, four)], [frule(1, dst="10.77.5.10/32")]),
        "exactly-one-rule": fixed([fpeer(0, ["10.77.2.0/24"], role="responder")], one),
        "exactly-128-rules": fixed([fpeer(0, ["10.77.2.0/24"])], many),
        "mixed-protocols-and-ports": fixed([fpeer(0, ["10.77.2.0/24"])], [
            frule(1, ports="1"), frule(2, ports="65535"), frule(3, ports="1000-2000"),
            frule(4, ports="1-65535"), frule(5, proto="udp", ports="53"),
            frule(6, proto="udp", ports="443-443"), frule(7, proto="icmp-echo"),
            frule(8, src="10.77.2.0/24", dst="10.77.1.0/24", ports="22")]),
        "four-peers-mixed-roles": fixed(
            [fpeer(i, [f"10.78.{i}.0/24", f"10.79.{i}.0/24"], pmtu=[1280, 1400, 1500, None][i],
                   role=["initiator", "responder"][i % 2]) for i in range(4)],
            [frule(i + 1, peer=f"peer{i}", dst=f"10.78.{i}.5/32") for i in range(4)]),
        "gateway-addresses-at-the-edges": fixed(
            [fpeer(0, ["10.77.2.0/24"])], [frule(1, src="192.168.255.254/32")],
            lan="192.168.0.1/16", und="172.31.0.2/16"),
    }


def check_fixtures_on_disk(models):
    """The checked-in TOML must be exactly what `render` makes, so the Rust test and this one agree."""
    ok = True
    for name, m in models.items():
        try:
            ok &= open(os.path.join(MUST_ACCEPT_DIR, name + ".toml")).read() == render(m, MUST_ACCEPT_KEY) + "\n"
        except OSError:
            ok = False
    have = set(os.listdir(MUST_ACCEPT_DIR)) if os.path.isdir(MUST_ACCEPT_DIR) else set()
    return ok and have == {n + ".toml" for n in models}


# ---- Kernel side ---------------------------------------------------------------------------------

class Kernel:
    def __init__(self):
        os.unshare(os.CLONE_NEWNET)
        os.system("ip link set lo up")
        if os.system("ip link add vpn0 type vpt1_ay1") != 0:
            sys.exit("cannot create vpn0 (is vpt1_foundation loaded?)")
        self.ifx = socket.if_nametoindex("vpn0")
        self.cid = bytes(range(1, 17))
        self.g = G.Genl()
        err, _ = self.g.cmd("CLAIM", *G.owner(self.ifx, self.cid))
        if err:
            sys.exit(f"claim failed: {err}")
        self.last_renew = time.time()

    def validate(self, payload):
        if time.time() - self.last_renew > 1.0:  # the lease is 5 s
            self.g.cmd("RENEW", *G.owner(self.ifx, self.cid), G.a("CONFIG_REVISION", G.u64(0)))
            self.last_renew = time.time()
        return self.g.cmd("VALIDATE", *G.owner(self.ifx, self.cid), G.a("CONFIG_REVISION", G.u64(1)),
                          G.attr(G.A["SNAPSHOT"], payload, nested=True))[0]


def faults():
    out = subprocess.run("dmesg | grep -E 'BUG:|WARNING:|Oops|KASAN|UBSAN|refcount_t|lockdep' || true",
                         shell=True, capture_output=True, text=True).stdout
    return out.strip()


# ---- Test ----------------------------------------------------------------------------------------

def vptctl(vpt, *args):
    return subprocess.run([vpt, *args], capture_output=True, text=True)


def reason(stderr):
    """Group controller refusals by the field and the rule, not by the values."""
    line = stderr.strip().splitlines()[-1] if stderr.strip() else "?"
    line = re.sub(r"peer\.[^.:\s]*\.", "peer.N.", line)
    line = re.sub(r"rule\.\d+|\[\d+\]|\d+", "#", line)
    return line[:90]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vptctl", nargs="?", default=os.environ.get("VPTCTL", ""))
    ap.add_argument("--count", type=int, default=1200)
    ap.add_argument("--seed", type=int, default=20261104)
    ap.add_argument("--no-kernel", action="store_true",
                    help="tune the generator without a kernel: every accepted snapshot counts as accepted")
    ap.add_argument("--write-must-accept", action="store_true",
                    help="write the fixed configurations as TOML into control/tests/vectors/must_accept, then exit")
    ap.add_argument("--keep", help="directory to keep every disagreement's configuration in")
    args = ap.parse_args()
    if args.write_must_accept:
        os.makedirs(MUST_ACCEPT_DIR, exist_ok=True)
        for name, m in must_accept().items():
            with open(os.path.join(MUST_ACCEPT_DIR, name + ".toml"), "w") as f:
                f.write(render(m, MUST_ACCEPT_KEY) + "\n")
        return
    if os.geteuid() != 0 and not args.no_kernel:
        sys.exit("run as root in a disposable VM")
    if not args.vptctl or not os.path.exists(args.vptctl):
        sys.exit("set VPTCTL to the vptctl binary")
    tmp = tempfile.mkdtemp(prefix="rj4val_")
    os.chmod(tmp, 0o700)
    keydir = os.path.join(tmp, "keys")
    os.makedirs(keydir)
    pubs, local_key = [], os.path.join(keydir, "local.key")
    local_pub = ""
    for name in ["local"] + [f"p{i}" for i in range(8)]:
        path = os.path.join(keydir, f"{name}.key")
        out = vptctl(args.vptctl, "genkey", "--out", path).stdout
        pub = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        if name == "local":
            local_pub = pub
        else:
            pubs.append(pub)
    kernel = Kernel() if not args.no_kernel else None
    before = faults() if kernel else ""

    if kernel is None:
        kernel = type("NoKernel", (), {"validate": staticmethod(lambda payload: 0)})()
    # Controls: the kernel can say no, and the sample is accepted by both.
    sample = open(os.path.join(REPO, "control/examples/gw-a.toml")).read().replace(
        "/etc/vpntunnel/gw-a.key", local_key)
    cfg = os.path.join(tmp, "sample.toml")
    open(cfg, "w").write(sample)
    r = vptctl(args.vptctl, "snapshot", cfg, "--lan-ifindex", str(LAN_IF), "--underlay-ifindex", str(UND_IF))
    check("sample-is-accepted-by-the-controller", r.returncode == 0, r.stderr[-200:])
    good = bytes.fromhex(r.stdout.strip()) if r.returncode == 0 else b""
    check("sample-is-accepted-by-the-kernel", good and kernel.validate(good) == 0)
    bad = bytearray(good)  # path MTU 1279 in the first peer record (member type 2): slot, role, pmtu
    off = 0
    while off + 4 <= len(bad):
        length, kind = struct.unpack_from("=HH", bad, off)
        if kind == 2:
            struct.pack_into(">H", bad, off + 4 + 2, 1279)
            break
        off += (length + 3) & ~3
    check("control-a-forged-path-mtu-is-refused-by-the-kernel", kernel.validate(bytes(bad)) == G.EINVAL,
          str(kernel.validate(bytes(bad))))

    # Fixed list: each must be accepted by the controller and by the kernel.
    fixed_models = must_accept()
    check("must-accept-fixtures-match-the-checked-in-toml", check_fixtures_on_disk(fixed_models),
          "run rj4_validate_corpus.py --write-must-accept")
    fixed_ok = 0
    for name, m in fixed_models.items():
        path = os.path.join(tmp, "fixed.toml")
        open(path, "w").write(render(m, local_key))
        r = vptctl(args.vptctl, "snapshot", path, "--lan-ifindex", str(LAN_IF),
                   "--underlay-ifindex", str(UND_IF))
        check(f"must-accept:{name}:controller", r.returncode == 0, r.stderr.strip()[-200:])
        code = kernel.validate(bytes.fromhex(r.stdout.strip())) if r.returncode == 0 else None
        check(f"must-accept:{name}:kernel", code == 0, str(code))
        fixed_ok += r.returncode == 0 and code == 0
    print(f"must-accept list: {fixed_ok}/{len(fixed_models)} accepted by both")

    rng = random.Random(args.seed)
    stats =collections.Counter()
    refused = collections.Counter()
    kernel_codes = collections.Counter()
    disagreements, accepted_payloads = [], []
    keep = args.keep or os.path.join(tmp, "disagreements")
    for n in range(args.count):
        m = None
        while m is None:
            try:
                m = build(rng, pubs, local_pub)
            except (TypeError, ValueError, IndexError, AttributeError):
                m = None  # the generator drew an impossible layout (for example no free host): draw again
        picks = rng.choices([0, 1, 2, 3], [30, 40, 22, 8])[0]
        names = []
        for fn in rng.sample(mutations(rng, m, pubs), picks):
            try:
                fn()
            except (IndexError, KeyError, ValueError, TypeError):
                continue  # an earlier mutation left nothing for this one to change
            names.append(fn.__name__)
        text = render(m, local_key)
        path = os.path.join(tmp, "c.toml")
        open(path, "w").write(text)
        r = vptctl(args.vptctl, "snapshot", path, "--lan-ifindex", str(LAN_IF), "--underlay-ifindex", str(UND_IF))
        stats["generated"] += 1
        if r.returncode != 0:
            stats["refused-by-controller"] += 1
            refused[reason(r.stderr)] += 1
            continue
        stats["accepted-by-controller"] += 1
        payload = bytes.fromhex(r.stdout.strip())
        accepted_payloads.append(payload)
        code = kernel.validate(payload)
        kernel_codes[code] += 1
        if code != 0:
            os.makedirs(keep, exist_ok=True)
            dest = os.path.join(keep, f"disagreement-{n}.toml")
            open(dest, "w").write(text)
            disagreements.append((n, names, code, dest))
        if names:
            stats[f"mutated-and-accepted:{names[0]}"] += 1

    print(f"corpus: seed {args.seed}, {stats['generated']} configurations, "
          f"{stats['accepted-by-controller']} accepted by the controller, "
          f"{stats['refused-by-controller']} refused")
    print("kernel verdicts on the accepted ones:", dict(kernel_codes))
    print(f"controller refusals by reason ({len(refused)} kinds, top 12):")
    for why, k in refused.most_common(12):
        print(f"  {k:5d}  {why}")
    for n, names, code, dest in disagreements[:10]:
        print(f"DISAGREEMENT #{n} errno {code} mutations {names} -> {dest}")
    check("every-configuration-the-controller-accepts-is-accepted-by-the-kernel", not disagreements,
          f"{len(disagreements)} of {stats['accepted-by-controller']}")
    check("the-corpus-accepts-a-meaningful-share",
          stats["accepted-by-controller"] >= args.count * 0.25, str(stats["accepted-by-controller"]))
    check("the-corpus-refuses-a-meaningful-share",
          stats["refused-by-controller"] >= args.count * 0.10, str(stats["refused-by-controller"]))
    check("the-corpus-reaches-many-different-refusals", len(refused) >= 30, str(len(refused)))
    check("boundary-configurations-were-accepted",
          sum(v for k, v in stats.items() if k.startswith("mutated-and-accepted")) >= args.count * 0.05)

    # Corrupted snapshots: any verdict, but no hang and no kernel fault.
    fuzz = collections.Counter()
    for payload in rng.sample(accepted_payloads, min(40, len(accepted_payloads))):
        for _ in range(10):
            b = bytearray(payload)
            how = rng.choice(["flip", "truncate", "extend", "length", "type"])
            if how == "flip":
                for _ in range(rng.randint(1, 4)):
                    b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
            elif how == "truncate":
                b = b[:rng.randrange(len(b))]
            elif how == "extend":
                b += os.urandom(rng.randint(1, 40))
            elif how == "length":
                struct.pack_into("=H", b, 0, rng.choice([0, 1, 3, 4, 5, 0xffff]))
            else:
                struct.pack_into("=H", b, 2, rng.choice([0, 3, 4, 99, 0xffff, 0x8001]))
            try:
                fuzz[kernel.validate(bytes(b))] += 1
            except OSError as e:
                fuzz[f"socket-error:{e.errno}"] += 1
    print("corrupted snapshots -> kernel verdicts:", dict(fuzz))
    check("corrupted-snapshots-were-judged-without-a-hang",
          not any(str(k).startswith("socket-error") for k in fuzz), str(dict(fuzz)))
    check("corrupted-snapshots-are-mostly-refused", fuzz[0] < sum(fuzz.values()) * 0.5, str(dict(fuzz)))
    after = faults()
    check("no-kernel-fault-markers", after == before, after[-300:])

    print(f"RJ4_VALIDATE_CORPUS {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed} "
          f"seed={args.seed} count={args.count}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
