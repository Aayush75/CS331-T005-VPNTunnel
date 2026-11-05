#!/usr/bin/env python3
"""Soak: the tunnel under continuous load while its keys keep changing.

Two `vptctl run` gateways re-key every 4 seconds. For the whole run, in both directions at once:
100 echo requests a second each way, and repeated 16 MiB TCP downloads. Nothing may be lost, every
download must arrive intact, every re-key must complete, and neither controller may log a failed
session or attempt. The module's leak check after the run (the suite's "no leaked objects") covers
what is left behind.

  VPT1_SOAK_SECS=45 (default)   how long to run; use 300 or more by hand
  VPTCTL=/path/to/vptctl ay4_soak.py     as root in a disposable VM with vpt1_foundation loaded
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_controller as CT  # noqa: E402  (controller helpers and the two-gateway configuration)

D = CT.D
SECS = int(os.environ.get("VPT1_SOAK_SECS", "45"))
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    if not CT.VPTCTL or not os.path.exists(CT.VPTCTL):
        sys.exit("set VPTCTL to the vptctl binary")
    r = CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="ay4soak_")
    os.chmod(tmp, 0o700)
    ctls, servers = [], []
    stop = threading.Event()
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
            out = CT.run(CT.VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a, cfg_b = os.path.join(tmp, "a.toml"), os.path.join(tmp, "b.toml")
        open(cfg_a, "w").write(CT.toml("a", pubs["a"], pubs["b"], "172.31.250.12", "10.77.2.0/24", "initiator",
                                       "veth_gwa_lan", "veth_gwa_out", "10.77.1.1/24", "172.31.250.11/24", keys["a"]))
        open(cfg_b, "w").write(CT.toml("b", pubs["b"], pubs["a"], "172.31.250.11", "10.77.1.0/24", "responder",
                                       "veth_gwb_lan", "veth_gwb_out", "10.77.2.1/24", "172.31.250.12/24", keys["b"]))
        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a.log"), rekey=4)
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b.log"))
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        check("tunnel-comes-up", CT.wait_for(CT.ping_ok, 20), ca.text()[-300:])

        www = os.path.join(tmp, "www")
        os.makedirs(www)
        blob = os.urandom(16 * 1024 * 1024)
        open(os.path.join(www, "blob"), "wb").write(blob)
        want = hashlib.sha256(blob).hexdigest()
        servers.append(subprocess.Popen(
            ["ip", "netns", "exec", D.NS["hb"], sys.executable, "-m", "http.server", "8080", "--bind", "10.77.2.10",
             "--directory", www], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.8)
        fetch = ("import hashlib,urllib.request;b=urllib.request.urlopen('http://10.77.2.10:8080/blob',timeout=60).read();"
                 "print(len(b),hashlib.sha256(b).hexdigest())")
        downloads = {"ok": 0, "bad": 0}

        def download_loop():
            while not stop.is_set():
                r = D.sh(sys.executable, "-c", fetch, ns="ha", timeout=120)
                out = r.stdout.split()
                if r.returncode == 0 and out == [str(len(blob)), want]:
                    downloads["ok"] += 1
                else:
                    downloads["bad"] += 1
                    print(f"    bad download: rc={r.returncode} {r.stdout[-80:]} {r.stderr[-120:]}")

        est0 = ca.text().count("established (initiator)")
        # A fixed duration, not a fixed count: under load ping sends fewer than 100 a second.
        pings = [subprocess.Popen(["ip", "netns", "exec", D.NS[src], "ping", "-w", str(SECS), "-i", "0.01", "-W", "2", dst],
                                  stdout=subprocess.PIPE, text=True)
                 for src, dst in (("ha", "10.77.2.10"), ("hb", "10.77.1.10"))]
        loader = threading.Thread(target=download_loop, daemon=True)
        loader.start()
        lost, sent = [], []
        for p in pings:
            m = re.search(r"(\d+) packets transmitted, (\d+) received", p.communicate(timeout=SECS + 60)[0])
            sent.append(int(m[1]) if m else 0)
            lost.append(int(m[1]) - int(m[2]) if m else 9999)
        stop.set()
        loader.join(timeout=150)
        est = ca.text().count("established (initiator)") - est0

        # The bulk data flows from B to A, so B's pings are sent far less often than 100 a second while
        # the downloads run (about 4 a second in a 240 s run); the check is only that pinging went on.
        check("pinged-for-the-whole-run", min(sent) >= SECS, f"{sent} pings sent")
        check("no-ping-lost-a-to-b", lost[0] == 0, f"{lost[0]} of {sent[0]} lost")
        check("no-ping-lost-b-to-a", lost[1] == 0, f"{lost[1]} of {sent[1]} lost")
        check("downloads-all-intact", downloads["bad"] == 0 and downloads["ok"] >= 3, f"{downloads}")
        check("re-keys-kept-happening", est >= SECS // 8, f"{est} re-keys in {SECS} s")
        for name, c in (("initiator", ca), ("responder", cb)):
            text = c.text()
            check(f"{name}-logged-no-failed-session-or-attempt",
                  "session ended" not in text and "timed out" not in text, text[-400:])
        check("tunnel-still-works-afterwards", CT.wait_for(CT.ping_ok, 10))
        print(f"    {SECS} s: {est} re-keys, {downloads['ok']} downloads of 16 MiB, {sent[0]} and {sent[1]} pings")
    finally:
        stop.set()
        for c in ctls:
            c.stop()
        for s in servers:
            s.terminate()
        CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"AY4_SOAK {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
