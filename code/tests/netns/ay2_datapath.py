#!/usr/bin/env python3
"""AY2 thin data path, end to end on a real kernel.

Builds the N1 fixture (two sites joined only by an underlay bridge), loads one
gateway per site through the real vptunnel Generic Netlink endpoint with static keys
and receive locators, then proves:

  * a ping and an HTTP request cross the tunnel with the original source address;
  * the underlay carries only UDP/51888 ciphertext: no private IPv4 packet and no
    request ID in the clear;
  * denied flows (no ACL rule) never arrive, in either direction;
  * a replayed, a tampered, a wrongly addressed, a spoofed-source and a garbage
    datagram are each dropped and counted, and none reaches the LAN;
  * removing a gateway fails closed.

Run as root inside a disposable VM with vpt1_foundation loaded:  ay2_datapath.py
"""
import contextlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_genl as G  # noqa: E402  (shared netlink helpers)

REPO = os.path.dirname(os.path.dirname(HERE))
RUN = f"dp{os.getpid()}"
NS = {n: f"vpt_{RUN}_{n}" for n in ("ha", "gwa", "und", "gwb", "hb")}
ID_A, ID_B = bytes(range(1, 17)), bytes(range(51, 67))
K1, K2 = bytes(range(32)), bytes(range(100, 132))

# counter ids: reasons 0..13 then events (see vpt1_gateway.h)
C = dict(TX_PACKETS=21, RX_PACKETS=22, TX_NO_ROUTE=23, TX_DROPPED=24, RX_MALFORMED=25,
         RX_UNKNOWN_LOCATOR=26, RX_BAD_SOURCE=27, RX_AUTH_FAIL=28, RX_REPLAY=29,
         RX_POLICY=30, RX_DROPPED=31, ACTIVATED=42, ACTIVATION_FAILED=43, RX_CANDIDATE_DROPPED=44,
         TX_ACTIVATION=45, RX_ACTIVATION=46, REKEY_GRANTS=47, REKEY_NEEDED=48)

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def sh(*argv, ns=None, timeout=30, check_rc=False):
    cmd = (["ip", "netns", "exec", NS[ns]] if ns else []) + list(argv)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if check_rc and r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stderr.strip()}")
    return r


@contextlib.contextmanager
def netns(name):
    """Create sockets inside a namespace: a socket keeps the namespace it was born in."""
    home = os.open("/proc/self/ns/net", os.O_RDONLY)
    target = os.open(f"/var/run/netns/{NS[name]}", os.O_RDONLY)
    try:
        os.setns(target, os.CLONE_NEWNET)
        yield
    finally:
        os.setns(home, os.CLONE_NEWNET)
        os.close(home)
        os.close(target)


def key(seed):
    return bytes((seed + i) & 255 for i in range(32))


def pfx(addr, length):
    return socket.inet_aton(addr) + bytes([length])


def snapshot(lan_if, und_if, lan_net, lan_addr, und_addr, peer_ep, peer_net, rules, own, peer,
             pmtu=1500):
    local = (struct.pack(">II", lan_if, und_if) + pfx(lan_net, 24) + pfx("172.31.250.0", 24) +
             socket.inet_aton(lan_addr) + socket.inet_aton(und_addr) + key(own))
    peer_rec = (struct.pack(">BBH", 1, 1 if own < peer else 0, pmtu) + socket.inet_aton(peer_ep) +
                struct.pack(">H", 51888) + key(peer) + bytes([1]) + pfx(peer_net, 24) + b"\0" * 15)
    out = G.attr(1, local) + G.attr(2, peer_rec)
    for rid, direction, proto, port, src, dst in rules:
        out += G.attr(3, struct.pack(">IBBBHH", rid, 1, direction, proto, port, port) +
                      pfx(src, 32) + pfx(dst, 32))
    return out


class Gateway:
    """One site's controller: owns the lease and renews it in the background."""

    def __init__(self, site, cid):
        self.site, self.cid = site, cid
        with netns(site):
            self.owner = G.Genl()
            self.observer = G.Genl()
            self.ifx = socket.if_nametoindex("vpn0")
            self.lan_if = socket.if_nametoindex("veth_gwa_lan" if site == "gwa" else "veth_gwb_lan")
            self.und_if = socket.if_nametoindex("veth_gwa_out" if site == "gwa" else "veth_gwb_out")
        self.stop = threading.Event()
        self.thread = None
        self.lock = threading.RLock()

    def cmd(self, name, *attrs):
        with self.lock:
            return self.owner.cmd(name, *G.owner(self.ifx, self.cid), *attrs)

    def begin_attempt(self, slot=1):
        inc = G.status(self.observer, self.ifx)[1]['peers'][slot]['inc']
        err, reply = self.cmd('BEGIN_ATTEMPT', G.a('PEER_SLOT', G.u16(slot)),
                              G.a('PEER_INCARNATION', inc))
        if err:
            raise RuntimeError(f'{self.site} BEGIN_ATTEMPT slot {slot}: {err}')
        return inc, reply[G.A['ATTEMPT_ID']][0], struct.unpack('=Q', reply[G.A['LOCAL_RX']][0])[0]

    def start_renewals(self):
        def loop():
            while not self.stop.wait(1.0):
                self.cmd("RENEW", G.a("CONFIG_REVISION", G.u64(1)))
        self.thread = threading.Thread(target=loop, daemon=True)
        self.thread.start()

    def counters(self):
        err, reply = self.observer.cmd("COUNTERS", G.a("IFINDEX", G.u32(self.ifx)))
        nested = G.parse_attrs(reply[G.A["COUNTERS"]][0])
        return {k: struct.unpack("=Q", nested[v + 1][0])[0] for k, v in C.items()}

    def incarnation(self):
        return G.status(self.observer, self.ifx)[1]["peers"][1]["inc"]


# gwb's receive locator, as the kernel issued it (set by whoever provisions both gateways).
GWB_RX = [0]


def prepare(gw, snap, after_commit=None):
    """Claim, configure, and ask the kernel for the attempt (its ID and receive locator)."""
    check(f"{gw.site}-claim", gw.owner.cmd("CLAIM", *G.owner(gw.ifx, gw.cid))[0] == 0)
    check(f"{gw.site}-validate", gw.cmd("VALIDATE", G.a("CONFIG_REVISION", G.u64(1)),
          G.attr(G.A["SNAPSHOT"], snap, nested=True))[0] == 0)
    err = gw.cmd("COMMIT_CONFIG", G.a("CONFIG_REVISION", G.u64(1)))[0]
    check(f"{gw.site}-commit-binds-socket", err == 0, f"err={err}")
    if err == 0 and after_commit is not None:
        after_commit()
    gw.inc, gw.attempt, gw.local_rx = gw.begin_attempt()
    check(f"{gw.site}-begin-attempt", True)


def finish(gw, tx, rx, remote_rx):
    """Install the keys for the kernel-issued attempt; the peer's locator is the other kernel's."""
    err = gw.cmd("STAGE_GENERATION", G.a("PEER_SLOT", G.u16(1)), G.a("PEER_INCARNATION", gw.inc),
                 G.a("ATTEMPT_ID", gw.attempt), G.a("HANDSHAKE_HASH", b"H" * 32),
                 G.a("LOCAL_RX", G.u64(gw.local_rx)), G.a("REMOTE_RX", G.u64(remote_rx)),
                 G.a("TX_KEY", tx), G.a("RX_KEY", rx), G.a("MODE", b"\0"))[0]
    check(f"{gw.site}-stage-generation", err == 0, f"err={err}")
    gw.start_renewals()


def wait_active(*gws, before=None, timeout=20):
    """The kernels activate a staged key themselves (CONFIRM, READY, COMMIT, COMMITTED); wait for
    both to have committed it. `before` is each site's ACTIVATED count when the keys were staged."""
    before = before or {}
    t0 = time.time()
    while time.time() - t0 < timeout:
        if all(g.counters()["ACTIVATED"] > before.get(g.site, 0) for g in gws):
            check("activated-" + "-".join(g.site for g in gws), True)
            return True
        time.sleep(0.1)
    check("activated-" + "-".join(g.site for g in gws), False,
          str({g.site: g.counters()["ACTIVATED"] for g in gws}))
    return False


# ---- pcap and raw datagram helpers ------------------------------------------------

def read_pcap(path):
    data = open(path, "rb").read()
    pkts, off = [], 24
    while off + 16 <= len(data):
        _, _, caplen, _ = struct.unpack_from("<IIII", data, off)
        off += 16
        pkts.append(data[off:off + caplen])
        off += caplen
    return pkts


def udp_datagrams(pkts, port=51888):
    """(src, dst, payload) for every IPv4/UDP frame to or from the tunnel port."""
    out = []
    for p in pkts:
        if len(p) < 42 or p[12:14] != b"\x08\x00":
            continue
        ihl = (p[14] & 15) * 4
        if p[14 + 9] != 17:
            continue
        sport, dport, ulen = struct.unpack_from(">HHH", p, 14 + ihl)
        if port not in (sport, dport):
            continue
        out.append((socket.inet_ntoa(p[26:30]), socket.inet_ntoa(p[30:34]),
                    p[14 + ihl + 8:14 + ihl + ulen]))
    return out


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f">{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def raw_udp(src, dst, payload, sport=51888, dport=51888):
    """One IPv4/UDP datagram with real checksums, for IP_HDRINCL."""
    udp_len = 8 + len(payload)
    pseudo = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack(">BBH", 0, 17, udp_len)
    hdr = struct.pack(">HHHH", sport, dport, udp_len, 0)
    c = csum(pseudo + hdr + payload) or 0xFFFF
    udp = struct.pack(">HHHH", sport, dport, udp_len, c) + payload
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0x4242, 0x4000, 64, 17, 0,
                     socket.inet_aton(src), socket.inet_aton(dst))
    ip = ip[:10] + struct.pack(">H", csum(ip)) + ip[12:]
    return ip + udp


def inject(site, src, dst, payload):
    with netns(site):
        s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
        s.sendto(raw_udp(src, dst, payload), (dst, 0))
        s.close()


@contextlib.contextmanager
def capture(site, ifname, path):
    # A previous run's pcap header must not satisfy the readiness check.
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    p = subprocess.Popen(["ip", "netns", "exec", NS[site], "tcpdump", "--immediate-mode", "-U", "-i",
                          ifname, "-n", "-s", "0", "-w", path], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            if p.poll() is not None:
                raise RuntimeError(f"tcpdump exited before capture: {p.returncode}")
            if os.path.exists(path) and os.path.getsize(path) >= 24:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError(f"tcpdump did not create a pcap header: {path}")
        yield
    finally:
        time.sleep(0.4)
        p.terminate()
        p.wait()


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    r = subprocess.run(["bash", os.path.join(HERE, "n1_fixture.sh"), "setup", RUN], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="ay2dp_")
    servers = []
    try:
        for site in ("gwa", "gwb"):
            sh("ip", "link", "add", "vpn0", "type", "vpt1_ay1", ns=site, check_rc=True)
            sh("ip", "link", "set", "vpn0", "up", ns=site, check_rc=True)
            sh("sysctl", "-q", "-w", "net.ipv4.conf.all.rp_filter=0", ns=site)
        sh("ip", "route", "add", "10.77.2.0/24", "dev", "vpn0", ns="gwa", check_rc=True)
        sh("ip", "route", "add", "10.77.1.0/24", "dev", "vpn0", ns="gwb", check_rc=True)

        ga, gb = Gateway("gwa", ID_A), Gateway("gwb", ID_B)
        rules_a = [(1, 1, 6, 8080, "10.77.1.10", "10.77.2.10"), (2, 1, 1, 0, "10.77.1.10", "10.77.2.10")]
        rules_b = [(1, 2, 6, 8080, "10.77.1.10", "10.77.2.10"), (2, 2, 1, 0, "10.77.1.10", "10.77.2.10")]
        prepare(ga, snapshot(ga.lan_if, ga.und_if, "10.77.1.0", "10.77.1.1", "172.31.250.11",
                             "172.31.250.12", "10.77.2.0", rules_a, 0x10, 0x20))
        prepare(gb, snapshot(gb.lan_if, gb.und_if, "10.77.2.0", "10.77.2.1", "172.31.250.12",
                             "172.31.250.11", "10.77.1.0", rules_b, 0x20, 0x10))
        finish(ga, K1, K2, gb.local_rx)
        finish(gb, K2, K1, ga.local_rx)
        wait_active(ga, gb)

        time.sleep(0.5)  # carrier follows the bound socket; the qdisc attaches asynchronously
        # Two HTTP services on host-b: 8080 is allowed by the rules, 9999 is not.
        for port in (8080, 9999):
            servers.append(subprocess.Popen(
                ["ip", "netns", "exec", NS["hb"], sys.executable,
                 os.path.join(REPO, "tools/oracles/http_server.py"), "--host", "10.77.2.10",
                 "--port", str(port), "--server-id", "host-b"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.7)

        def http(port, ns="ha", timeout=2.0, expect_ip="10.77.1.10"):
            return sh(sys.executable, os.path.join(REPO, "tools/oracles/http_client.py"),
                      f"http://10.77.2.10:{port}", "--expect-server", "host-b",
                      "--expect-client-ip", expect_ip, "--timeout", str(timeout), ns=ns, timeout=20)

        base_a, base_b = ga.counters(), gb.counters()
        pcap = os.path.join(tmp, "und_a.pcap")
        with capture("und", "veth_und_a", pcap):
            ping = sh("ping", "-c", "3", "-W", "2", "10.77.2.10", ns="ha", timeout=30)
            check("ping-crosses-the-tunnel", ping.returncode == 0, ping.stdout[-200:])
            good = http(8080)
            check("http-crosses-with-original-source", good.returncode == 0, good.stdout[-300:])
            req_id = ""
            if good.returncode == 0:
                import json
                req_id = json.loads(good.stdout)["sent_request_id"]
            denied = http(9999, timeout=1.5)
            check("denied-port-never-arrives", denied.returncode != 0 and "timed out" in denied.stdout)
            rev = sh(sys.executable, os.path.join(REPO, "tools/oracles/http_client.py"),
                     "http://10.77.1.10:8080", "--timeout", "1.5", ns="hb", timeout=20)
            check("peer-initiated-flow-denied", rev.returncode != 0)
        now_a, now_b = ga.counters(), gb.counters()
        check("tx-and-rx-counted", now_a["TX_PACKETS"] > base_a["TX_PACKETS"] and
              now_a["RX_PACKETS"] > base_a["RX_PACKETS"] and now_b["RX_PACKETS"] > base_b["RX_PACKETS"] and
              now_b["TX_PACKETS"] > base_b["TX_PACKETS"], f"{now_a} {now_b}")
        check("denied-flow-counted-at-the-sender", now_a["TX_DROPPED"] > base_a["TX_DROPPED"])

        # The underlay carries ciphertext only.
        pkts = read_pcap(pcap)
        dgrams = udp_datagrams(pkts)
        private = [p for p in pkts if len(p) > 34 and p[12:14] == b"\x08\x00" and
                   (p[26:28] == b"\x0a\x4d" or p[30:32] == b"\x0a\x4d")]
        check("underlay-has-tunnel-datagrams", len(dgrams) >= 6, f"{len(dgrams)}")
        check("no-private-ipv4-on-the-underlay", not private, f"{len(private)}")
        check("request-id-not-in-the-clear", req_id and req_id.encode() not in open(pcap, "rb").read())
        records = [d for d in dgrams if d[2][:5] == b"VPT1\x03" and d[0] == "172.31.250.11"]
        check("records-have-the-vpt1-envelope", len(records) >= 3)

        # Hostile datagrams at gateway B. Each must be dropped, counted, and never delivered.
        pre = gb.counters()
        lan_cap = os.path.join(tmp, "lanb.pcap")
        victim = records[0][2]
        with capture("hb", "veth_hb", lan_cap):
            inject("gwa", "172.31.250.11", "172.31.250.12", victim)  # replay of a sent record
            # A fresh (never seen) counter so the replay check passes and authentication
            # itself must reject the record: the counter is part of the authenticated data.
            ctr = struct.unpack(">Q", victim[16:24])[0]
            forged = bytearray(victim)
            forged[16:24] = struct.pack(">Q", ctr + 5000)
            inject("gwa", "172.31.250.11", "172.31.250.12", bytes(forged))  # counter not authenticated
            tampered = bytearray(victim)
            tampered[16:24] = struct.pack(">Q", ctr + 5001)
            tampered[-1] ^= 1
            inject("gwa", "172.31.250.11", "172.31.250.12", bytes(tampered))  # bad tag, fresh counter
            wrong_loc = bytearray(victim)
            wrong_loc[15] ^= 1
            inject("gwa", "172.31.250.11", "172.31.250.12", bytes(wrong_loc))  # unknown locator
            inject("gwa", "172.31.250.99", "172.31.250.12", victim)  # spoofed outer source
            inject("gwa", "172.31.250.11", "172.31.250.12", b"garbage")  # not VPT1
            time.sleep(0.5)
        post = gb.counters()
        delta = {k: post[k] - pre[k] for k in C}
        check("replay-dropped", delta["RX_REPLAY"] == 1, str(delta))
        check("tampered-records-fail-authentication", delta["RX_AUTH_FAIL"] == 2, str(delta))
        check("unknown-locator-dropped", delta["RX_UNKNOWN_LOCATOR"] == 1, str(delta))
        check("spoofed-source-dropped", delta["RX_BAD_SOURCE"] == 1, str(delta))
        check("garbage-dropped", delta["RX_MALFORMED"] == 1, str(delta))
        check("nothing-hostile-was-delivered", delta["RX_PACKETS"] == 0, str(delta))
        lan_ip = [p for p in read_pcap(lan_cap) if len(p) > 34 and p[12:14] == b"\x08\x00"]
        check("nothing-reached-the-lan", not lan_ip, f"{len(lan_ip)} frames")

        # Removing a gateway fails closed: no tunnel, no traffic.
        ga.stop.set()
        time.sleep(1.2)
        check("remove-gateway", ga.cmd("REMOVE_GATEWAY")[0] == 0)
        after = sh("ping", "-c", "2", "-W", "1", "10.77.2.10", ns="ha")
        check("no-traffic-after-gateway-removal", after.returncode != 0)
        check("vpn0-stays-up", sh("ip", "link", "show", "vpn0", ns="gwa").returncode == 0)
    finally:
        for s in servers:
            s.terminate()
        subprocess.run(["bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", RUN], capture_output=True)
    print(f"AY2_DATAPATH {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
