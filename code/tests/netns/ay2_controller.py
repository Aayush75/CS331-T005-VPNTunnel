#!/usr/bin/env python3
"""The Rust controller (`vptctl run`) drives the real kernel module end to end.

No test code provisions anything: two `vptctl run` processes, one per gateway namespace,
claim their gateways, load the configuration, run the Noise exchange through the kernel
(EXCHANGE_TX / EXCHANGE_RX), install the keys, keep the lease, and the tunnel comes up.
Then the same traffic checks as ay2_datapath.py, plus: re-keying without losing traffic,
the controller dying (admission closes at once, fail closed), restarting, and a forged
INIT that must change nothing.

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl ay2_controller.py
"""
import contextlib
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_datapath as D  # noqa: E402  (fixture, capture and traffic helpers)

G = D.G
VPTCTL = os.environ.get("VPTCTL") or (sys.argv[1] if len(sys.argv) > 1 else "")
REPO = D.REPO
passed = failed = 0
C2 = dict(TX_EXCHANGE=32, RX_EXCHANGE=33, RX_EXCHANGE_DROPPED=34, TX_PACKETS=21, RX_PACKETS=22,
          RX_BAD_SOURCE=27)


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def run(*argv, **kw):
    return subprocess.run(list(argv), capture_output=True, text=True, **kw)


def toml(site, pub_self_unused, peer_pub, peer_ep, peer_net, role, lan_if, und_if, lan_addr, und_addr, key_file):
    return f'''[gateway]
name = "gw-{site}"
private_key_file = "{key_file}"
lan_interface = "{lan_if}"
lan_address = "{lan_addr}"
underlay_interface = "{und_if}"
underlay_address = "{und_addr}"

[[peer]]
name = "peer"
slot = 1
public_key = "{peer_pub}"
endpoint = "{peer_ep}"
role = "{role}"
path_mtu = 1500
prefixes = ["{peer_net}"]

[[rule]]
id = 1
peer = "peer"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8080"

[[rule]]
id = 2
peer = "peer"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "icmp-echo"

[[rule]]
id = 3
peer = "peer"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8081"

[[rule]]
id = 4
peer = "peer"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "udp"
ports = "5000"

[[rule]]
id = 5
peer = "peer"
source = "10.77.2.10/32"
destination = "10.77.1.10/32"
protocol = "icmp-echo"
'''


class Controller:
    def __init__(self, site, cfg, log_path, rekey=None, extra=()):
        self.site, self.cfg, self.log_path, self.rekey = site, cfg, log_path, rekey
        self.extra = list(extra)
        self.proc = None

    def start(self):
        cmd = ["ip", "netns", "exec", D.NS[self.site], VPTCTL, "run", self.cfg]
        if self.rekey:
            cmd += ["--rekey-after-secs", str(self.rekey)]
        cmd += self.extra
        self.log = open(self.log_path, "a")
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=self.log)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def text(self):
        self.log.flush()
        return open(self.log_path).read()


def ping_ok(count=1, wait=1):
    return D.sh("ping", "-c", str(count), "-W", str(wait), "10.77.2.10", ns="ha", timeout=30).returncode == 0


def wait_for(cond, seconds, step=0.25):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(step)
    return cond()


def counters(site):
    with D.netns(site):
        g = G.Genl()
        ifx = D.socket.if_nametoindex("vpn0")
    err, reply = g.cmd("COUNTERS", G.a("IFINDEX", G.u32(ifx)))
    nested = G.parse_attrs(reply[G.A["COUNTERS"]][0])
    out = {k: D.struct.unpack("=Q", nested[v + 1][0])[0] for k, v in C2.items()}
    g.close()
    return out


def flush_route_caches():
    for site in ("ha", "hb", "gwa", "gwb"):
        D.sh("ip", "route", "flush", "cache", ns=site)


def download(web_blob_sha, secs):
    """Fetch the 4 MiB blob from host-b on port 8081; True if it arrived intact in time."""
    code = ("import hashlib,urllib.request;"
            f"b=urllib.request.urlopen('http://10.77.2.10:8081/blob',timeout={secs}).read();"
            "print(len(b),hashlib.sha256(b).hexdigest())")
    r = D.sh(sys.executable, "-c", code, ns="ha", timeout=secs + 30)
    out = r.stdout.split()
    return r.returncode == 0 and len(out) == 2 and out[0] == str(4 * 1024 * 1024) and out[1] == web_blob_sha


def udp_delivered(sizes):
    """Send one datagram of each size from host-a to host-b port 5000; return the sizes received."""
    receiver = (
        "import socket, time\n"
        "s = socket.socket(2, 2)\n"
        "s.bind(('10.77.2.10', 5000))\n"
        "s.settimeout(0.5)\n"
        "seen = []\n"
        "end = time.time() + 3\n"
        "while time.time() < end:\n"
        "    try:\n"
        "        seen.append(len(s.recvfrom(65535)[0]))\n"
        "    except Exception:\n"
        "        pass\n"
        "print(seen)\n")
    recv = subprocess.Popen(["ip", "netns", "exec", D.NS["hb"], sys.executable, "-c", receiver],
                            stdout=subprocess.PIPE, text=True)
    time.sleep(0.6)
    sender = (
        "import socket\n"
        "s = socket.socket(2, 2)\n"
        "s.bind(('10.77.1.10', 0))\n"
        f"for n in {tuple(sizes)}:\n"
        "    s.sendto(b'x' * n, ('10.77.2.10', 5000))\n")
    D.sh(sys.executable, "-c", sender, ns="ha")
    return json.loads(recv.communicate(timeout=15)[0].strip() or "[]")


def packet_size_checks(tmp):
    """The tunnel's MTU is 1428: path-MTU discovery, the MSS clamp, and the fragment limit."""
    import hashlib
    web = os.path.join(tmp, "www")
    os.makedirs(web, exist_ok=True)
    blob = os.urandom(4 * 1024 * 1024)
    open(os.path.join(web, "blob"), "wb").write(blob)
    want = hashlib.sha256(blob).hexdigest()
    srv = subprocess.Popen(["ip", "netns", "exec", D.NS["hb"], sys.executable, "-m", "http.server", "8081",
                            "--bind", "10.77.2.10", "--directory", web],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.8)
        top = D.sh("ping", "-c", "2", "-W", "1", "-M", "do", "-s", "1400", "10.77.2.10", ns="ha")
        over = D.sh("ping", "-c", "2", "-W", "1", "-M", "do", "-s", "1401", "10.77.2.10", ns="ha")
        check("ping-at-the-tunnel-mtu-passes", top.returncode == 0, top.stdout[-200:])
        check("ping-over-the-tunnel-mtu-is-refused-with-the-mtu", over.returncode != 0)
        flush_route_caches()
        check("bulk-tcp-completes-through-path-mtu-discovery", download(want, 20))

        # A network that drops ICMP breaks path-MTU discovery: the transfer stalls.
        for site in ("gwa", "gwb"):
            D.sh("nft", "add", "table", "inet", "vpt_bh", ns=site, check_rc=True)
            D.sh("nft", "add", "chain", "inet", "vpt_bh", "out", "{ type filter hook output priority 0 ; }", ns=site, check_rc=True)
            D.sh("nft", "add", "rule", "inet", "vpt_bh", "out", "icmp", "type", "destination-unreachable", "drop", ns=site, check_rc=True)
        flush_route_caches()
        check("bulk-tcp-stalls-when-icmp-is-dropped-and-nothing-clamps", not download(want, 6))
        # The MSS clamp rule shipped for gateways removes the dependency on ICMP.
        for site in ("gwa", "gwb"):
            D.sh("nft", "-f", os.path.join(REPO, "infra/gateway/mss-clamp.nft"), ns=site, check_rc=True)
        flush_route_caches()
        check("mss-clamp-fixes-the-black-hole", download(want, 20))
        for site in ("gwa", "gwb"):
            D.sh("nft", "delete", "table", "inet", "vpt_bh", ns=site)
            D.sh("nft", "delete", "table", "inet", "vpt_mss", ns=site)
        flush_route_caches()

        # Datagrams that fit pass; ones the stack must fragment are dropped (fail closed):
        # fragment handling is not implemented (HR3), and a non-first fragment has no ports.
        got = udp_delivered((200, 1000, 1400, 2000, 4000))
        check("udp-that-fits-the-mtu-is-delivered", {200, 1000, 1400} <= set(got), str(got))
        check("udp-needing-fragmentation-is-dropped", not ({2000, 4000} & set(got)), str(got))
    finally:
        srv.terminate()


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    if not VPTCTL or not os.path.exists(VPTCTL):
        sys.exit("set VPTCTL to the vptctl binary")
    r = run("bash", os.path.join(HERE, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="ay2ctl_")
    os.chmod(tmp, 0o700)
    ctls, servers = [], []
    try:
        for site in ("gwa", "gwb"):
            D.sh("ip", "link", "add", "vpn0", "type", "vpt1_ay1", ns=site, check_rc=True)
            D.sh("ip", "link", "set", "vpn0", "up", ns=site, check_rc=True)
            D.sh("sysctl", "-q", "-w", "net.ipv4.conf.all.rp_filter=0", ns=site)
        D.sh("ip", "route", "add", "10.77.2.0/24", "dev", "vpn0", ns="gwa", check_rc=True)
        D.sh("ip", "route", "add", "10.77.1.0/24", "dev", "vpn0", ns="gwb", check_rc=True)

        keys, pubs = {}, {}
        for who in ("a", "b"):
            keys[who] = os.path.join(tmp, f"{who}.key")
            out = run(VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a = os.path.join(tmp, "a.toml")
        cfg_b = os.path.join(tmp, "b.toml")
        open(cfg_a, "w").write(toml("a", pubs["a"], pubs["b"], "172.31.250.12", "10.77.2.0/24", "initiator",
                                    "veth_gwa_lan", "veth_gwa_out", "10.77.1.1/24", "172.31.250.11/24", keys["a"]))
        open(cfg_b, "w").write(toml("b", pubs["b"], pubs["a"], "172.31.250.11", "10.77.1.0/24", "responder",
                                    "veth_gwb_lan", "veth_gwb_out", "10.77.2.1/24", "172.31.250.12/24", keys["b"]))
        for cfg in (cfg_a, cfg_b):
            check("config-validates", run(VPTCTL, "check-config", cfg, "--with-key").returncode == 0)

        for port in (8080, 9999):
            servers.append(subprocess.Popen(
                ["ip", "netns", "exec", D.NS["hb"], sys.executable, os.path.join(REPO, "tools/oracles/http_server.py"),
                 "--host", "10.77.2.10", "--port", str(port), "--server-id", "host-b"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.6)

        # Before the controllers run there is no tunnel at all.
        check("no-tunnel-before-the-controllers-run", not ping_ok())

        pcap = os.path.join(tmp, "und.pcap")
        ca = Controller("gwa", cfg_a, os.path.join(tmp, "a.log"), rekey=8)
        cb = Controller("gwb", cfg_b, os.path.join(tmp, "b.log"))
        ctls += [ca, cb]
        with D.capture("und", "veth_und_a", pcap):
            cb.start()
            time.sleep(0.3)
            ca.start()
            up = wait_for(ping_ok, 20)
            check("tunnel-comes-up-with-no-manual-provisioning", up, ca.text()[-400:] + cb.text()[-400:])
            check("initiator-logged-the-exchange", "established (initiator)" in ca.text(), ca.text()[-300:])
            check("responder-logged-the-exchange", "established (responder)" in cb.text(), cb.text()[-300:])
            ping = D.sh("ping", "-c", "3", "-W", "2", "10.77.2.10", ns="ha", timeout=30)
            check("ping-crosses-the-tunnel", ping.returncode == 0)
            ok = D.sh(sys.executable, os.path.join(REPO, "tools/oracles/http_client.py"), "http://10.77.2.10:8080",
                      "--expect-server", "host-b", "--expect-client-ip", "10.77.1.10", "--timeout", "3", ns="ha", timeout=30)
            check("http-crosses-with-original-source", ok.returncode == 0, ok.stdout[-300:])
            req_id = json.loads(ok.stdout)["sent_request_id"] if ok.returncode == 0 else ""
            bad = D.sh(sys.executable, os.path.join(REPO, "tools/oracles/http_client.py"), "http://10.77.2.10:9999",
                       "--timeout", "1.5", ns="ha", timeout=30)
            check("denied-port-never-arrives", bad.returncode != 0 and "timed out" in bad.stdout)
        pkts = D.read_pcap(pcap)
        private = [p for p in pkts if len(p) > 34 and p[12:14] == b"\x08\x00" and
                   (p[26:28] == b"\x0a\x4d" or p[30:32] == b"\x0a\x4d")]
        dgrams = D.udp_datagrams(pkts)
        kinds = {d[2][4] for d in dgrams if d[2][:4] == b"VPT1"}
        check("underlay-has-handshake-and-records", {1, 2, 3} <= kinds, f"kinds={kinds}")
        check("no-private-ipv4-on-the-underlay", not private, f"{len(private)}")
        check("request-id-not-in-the-clear", req_id and req_id.encode() not in open(pcap, "rb").read())
        # INIT and RESPONSE are cleartext Noise messages: neither contains a static key.
        for pub in pubs.values():
            check("no-static-public-key-on-the-wire", bytes.fromhex(pub) not in open(pcap, "rb").read())

        a, b = counters("gwa"), counters("gwb")
        check("kernel-sent-and-received-the-exchange", a["TX_EXCHANGE"] >= 1 and b["RX_EXCHANGE"] >= 1 and
              b["TX_EXCHANGE"] >= 1 and a["RX_EXCHANGE"] >= 1, f"{a} {b}")

        # Re-keying: a fresh exchange every 8 s here. The underlay gets 20 ms of delay each way, so a key
        # one side holds and the other does not yet is a window of tens of milliseconds, not microseconds.
        # Traffic in both directions, 100 packets a second for 20 s (two or more re-keys), must lose
        # nothing: new keys receive at once and sending switches only once the other side can read it (AY4).
        delay = [D.sh("tc", "qdisc", "add", "dev", dev, "root", "netem", "delay", "20ms", ns="und")
                 for dev in ("veth_und_a", "veth_und_b")]
        check("latency-added-on-the-underlay", all(r.returncode == 0 for r in delay), delay[0].stderr[-100:])
        est_before = ca.text().count("established (initiator)")
        pings = [subprocess.Popen(["ip", "netns", "exec", D.NS[src], "ping", "-c", "2000", "-i", "0.01", "-W", "2", dst],
                                  stdout=subprocess.PIPE, text=True)
                 for src, dst in (("ha", "10.77.2.10"), ("hb", "10.77.1.10"))]
        lost = []
        for p in pings:
            m = re.search(r"(\d+) packets transmitted, (\d+) received", p.communicate(timeout=90)[0])
            lost.append(int(m[1]) - int(m[2]) if m else 999)
        est_after = ca.text().count("established (initiator)")
        for dev in ("veth_und_a", "veth_und_b"):
            D.sh("tc", "qdisc", "del", "dev", dev, "root", ns="und")
        check("initiator-re-keyed-while-running", est_after >= est_before + 2, f"{est_before}->{est_after}")
        check("no-packet-lost-across-rekeying-a-to-b", lost[0] == 0, f"{lost[0]} of 2000 lost")
        check("no-packet-lost-across-rekeying-b-to-a", lost[1] == 0, f"{lost[1]} of 2000 lost")

        # A forged INIT (valid shape, random Noise bytes) from the peer's address changes nothing.
        forged = b"VPT1\x01\x00" + D.struct.pack(">H", 208) + bytes(8) + os.urandom(208)
        D.inject("gwa", "172.31.250.11", "172.31.250.12", forged)
        time.sleep(0.8)
        check("forged-init-rejected-by-the-controller", "rejected INIT" in cb.text(), cb.text()[-300:])
        check("tunnel-unaffected-by-the-forgery", ping_ok(2))

        # The controller dying releases its socket: admission closes at once (fail closed).
        ca.stop()
        check("traffic-stops-when-the-controller-dies", wait_for(lambda: not ping_ok(), 3))
        # Restarting claims a fresh lease and re-establishes without any help.
        ca.start()
        check("tunnel-returns-after-the-controller-restarts", wait_for(ping_ok, 20), ca.text()[-300:])
    finally:
        for c in ctls:
            c.stop()
        for s in servers:
            s.terminate()
        run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"AY2_CONTROLLER {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
