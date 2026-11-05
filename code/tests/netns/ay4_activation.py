#!/usr/bin/env python3
"""Activation and granted re-key on a real kernel, with the real controllers (AY4).

Two gateways on the N1 fixture, each with a vptctl process, the initiator re-keying every 3 seconds. Proves:

  - the first session is activated by the kernels themselves (CONFIRM, READY, COMMIT, COMMITTED): both
    count an activation, and application data flows only afterwards;
  - every later re-key asks for a continuity grant first and sends a mode 1 INIT, both kernels count the
    grant, and each re-key is activated the same way;
  - a continuous ping across several re-keys loses nothing (the grant keeps the flows and the commit
    switches sending only when both sides hold the keys);
  - no activation failed and no liveness closure happened on the way.

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl ay4_activation.py
"""
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_controller as CT  # noqa: E402  (controller helpers and the two-gateway configuration)

D = CT.D
D.C.update(TX_PING=36, RX_PING=37, TX_PONG=38, RX_PONG=39, RX_CONTROL_DROPPED=40, LIVENESS_LOST=41)
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
    tmp = tempfile.mkdtemp(prefix="ay4act_")
    os.chmod(tmp, 0o700)
    ctls, procs = [], []
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
        ga, gb = D.Gateway("gwa", D.ID_A), D.Gateway("gwb", D.ID_B)  # only for the counters
        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a.log"), rekey=3)
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b.log"))
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        check("tunnel-comes-up", CT.wait_for(CT.ping_ok, 25), ca.text()[-300:])
        check("first-session-activated-by-the-kernels", CT.wait_for(
            lambda: ga.counters()["ACTIVATED"] >= 1 and gb.counters()["ACTIVATED"] >= 1, 10),
            f"{ga.counters()['ACTIVATED']}/{gb.counters()['ACTIVATED']}")
        a0, b0 = ga.counters(), gb.counters()
        check("activation-records-were-exchanged",
              a0["TX_ACTIVATION"] >= 2 and b0["TX_ACTIVATION"] >= 2 and a0["RX_ACTIVATION"] >= 2 and
              b0["RX_ACTIVATION"] >= 2, f"{a0['TX_ACTIVATION']}/{b0['TX_ACTIVATION']}")
        check("the-first-session-was-fresh-not-granted", "granted re-key INIT" not in ca.text())

        # Continuous traffic across several re-keys, one ping every 100 ms, none may be lost.
        ping = subprocess.Popen(["ip", "netns", "exec", D.NS["ha"], "ping", "-i", "0.1", "-W", "1", "-c", "800",
                                 "10.77.2.10"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        procs.append(ping)
        out, _ = ping.communicate(timeout=120)
        loss = re.search(r"(\d+)% packet loss", out)
        check("no-ping-lost-across-the-re-keys", loss is not None and int(loss.group(1)) == 0,
              loss.group(0) if loss else out[-200:])
        a1, b1 = ga.counters(), gb.counters()
        grants = ca.text().count("sent granted re-key INIT")
        check("several-re-keys-ran-as-granted-ones", grants >= 3, f"{grants} granted INITs")
        check("both-kernels-counted-the-grants", a1["REKEY_GRANTS"] >= 3 and b1["REKEY_GRANTS"] >= 3,
              f"{a1['REKEY_GRANTS']}/{b1['REKEY_GRANTS']}")
        check("each-re-key-was-activated",
              a1["ACTIVATED"] - a0["ACTIVATED"] >= 3 and b1["ACTIVATED"] - b0["ACTIVATED"] >= 3,
              f"{a1['ACTIVATED'] - a0['ACTIVATED']}/{b1['ACTIVATED'] - b0['ACTIVATED']}")
        check("no-fresh-fallback-was-needed", "making a fresh attempt" not in ca.text(), ca.text()[-300:])
        check("no-activation-failed", a1["ACTIVATION_FAILED"] == 0 and b1["ACTIVATION_FAILED"] == 0,
              f"{a1['ACTIVATION_FAILED']}/{b1['ACTIVATION_FAILED']}")
        check("no-liveness-closure", a1["LIVENESS_LOST"] == 0 and b1["LIVENESS_LOST"] == 0)
        check("tunnel-still-works", CT.wait_for(CT.ping_ok, 10), ca.text()[-300:])
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for c in ctls:
            c.stop()
        CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"AY4_ACTIVATION {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
