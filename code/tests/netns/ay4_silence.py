#!/usr/bin/env python3
"""A restarted peer is noticed in seconds, not after the 90 second age timer.

The responder's controller is killed (no goodbye), its gateway closes when the lease runs out, and it
is started again. The initiator's keys mean nothing to it any more, so records sent under them go
nowhere. Without a liveness check the initiator would keep sending until its own 90 second re-key
timer fired. The controller watches the device's counters (`usage::Silence`): records keep leaving
and nothing comes back for 6 seconds, so it asks for new keys.

The age timer is left at its default of 90 seconds, so only this check can bring the tunnel back in
time. A healthy tunnel must not trigger it.

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl ay4_silence.py
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
SILENT = "no records from the peer while sending"
RECOVERY_LIMIT = 30.0
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
    tmp = tempfile.mkdtemp(prefix="ay4silence_")
    os.chmod(tmp, 0o700)
    ctls, background = [], []
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
        # The default timers: a re-key by age is 90 s away.
        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a.log"))
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b.log"))
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        check("tunnel-comes-up", CT.wait_for(CT.ping_ok, 20), ca.text()[-300:])
        est0 = ca.text().count("established (initiator)")

        # Traffic from A the whole time, answered by B while it is healthy.
        background.append(subprocess.Popen(
            ["ip", "netns", "exec", D.NS["ha"], "ping", "-q", "-i", "0.2", "-W", "1", "10.77.2.10"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

        # 1. A healthy tunnel with traffic never looks silent.
        time.sleep(14)
        check("healthy-tunnel-is-left-alone", SILENT not in ca.text(), ca.text()[-300:])

        # 2. B's controller dies without a word; its gateway closes when the lease expires.
        cb.proc.kill()
        cb.proc.wait()
        check("traffic-stops-once-the-responder-is-gone", CT.wait_for(lambda: not CT.ping_ok(), 15))
        time.sleep(1)

        # 3. B comes back with no memory of the old keys.
        cb.start()
        t0 = time.time()
        recovered = CT.wait_for(CT.ping_ok, RECOVERY_LIMIT, step=0.5)
        took = time.time() - t0
        check("tunnel-recovers-within-seconds-not-the-age-timer", recovered,
              f"not back after {RECOVERY_LIMIT:.0f} s")
        check("the-initiator-said-why", SILENT in ca.text(), ca.text()[-400:])
        check("it-re-established-with-new-keys", ca.text().count("established (initiator)") > est0)
        if recovered:
            print(f"    back {took:.1f} s after the responder restarted (age timer: 90 s)")
        check("tunnel-still-works", CT.wait_for(CT.ping_ok, 10))
    finally:
        for p in background:
            p.terminate()
        for c in ctls:
            c.stop()
        CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"AY4_SILENCE {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
