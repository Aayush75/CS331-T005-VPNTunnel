#!/usr/bin/env python3
"""The controller re-keys when a key generation has carried a lot of traffic (AY4).

The age timer is left at its default of 90 seconds, so only key usage can cause a re-key here. The
thresholds are lowered for the test (8 MB, 2000 records). Three things must hold:

  - an idle tunnel does not re-key early,
  - a bulk TCP download re-keys several times along the way and arrives intact, with no stall,
  - a burst of small packets (records, not bytes) triggers a re-key as well.

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl ay4_usage_rekey.py
"""
import hashlib
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
USAGE_LOG = "key usage reached a limit"
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
    tmp = tempfile.mkdtemp(prefix="ay4usage_")
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
            out = CT.run(CT.VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a, cfg_b = os.path.join(tmp, "a.toml"), os.path.join(tmp, "b.toml")
        open(cfg_a, "w").write(CT.toml("a", pubs["a"], pubs["b"], "172.31.250.12", "10.77.2.0/24", "initiator",
                                       "veth_gwa_lan", "veth_gwa_out", "10.77.1.1/24", "172.31.250.11/24", keys["a"]))
        open(cfg_b, "w").write(CT.toml("b", pubs["b"], pubs["a"], "172.31.250.11", "10.77.1.0/24", "responder",
                                       "veth_gwb_lan", "veth_gwb_out", "10.77.2.1/24", "172.31.250.12/24", keys["b"]))
        limits = ["--rekey-after-bytes", "8000000", "--rekey-after-records", "2000"]
        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a.log"), extra=limits)
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b.log"), extra=limits)
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        check("tunnel-comes-up", CT.wait_for(CT.ping_ok, 20), ca.text()[-300:])

        def usage_rekeys():
            return ca.text().count(USAGE_LOG)

        def established():
            return ca.text().count("established (initiator)")

        # 1. Idle: well past the one-second monitor, nothing to re-key for. (The count is read after the
        # first establishment settled; a single ping is far below both limits.)
        time.sleep(1.5)
        est0, use0 = established(), usage_rekeys()
        time.sleep(6)
        check("idle-tunnel-does-not-re-key", usage_rekeys() == use0 and established() == est0,
              f"{use0}->{usage_rekeys()} usage, {est0}->{established()} established")

        # 2. Bulk download: 48 MiB, six times the byte limit, arrives intact while the keys change.
        www = os.path.join(tmp, "www")
        os.makedirs(www)
        blob = os.urandom(48 * 1024 * 1024)
        open(os.path.join(www, "blob"), "wb").write(blob)
        want = hashlib.sha256(blob).hexdigest()
        servers.append(subprocess.Popen(
            ["ip", "netns", "exec", D.NS["hb"], sys.executable, "-m", "http.server", "8080", "--bind", "10.77.2.10",
             "--directory", www], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.8)
        # Keep traffic active across multiple one-second observations even on a
        # fast VM. Byte volume alone cannot guarantee three monitor ticks.
        fetch = """import hashlib,time,urllib.request
t=time.monotonic()
h=hashlib.sha256()
size=0
with urllib.request.urlopen('http://10.77.2.10:8080/blob',timeout=60) as response:
    while True:
        chunk=response.read(1024*1024)
        if not chunk:
            break
        size+=len(chunk)
        h.update(chunk)
        time.sleep(0.05)
print(size,h.hexdigest(),round(time.monotonic()-t,2))
"""
        # The one-rule policy of CT.toml allows TCP 8080 from the one laptop address only.
        est1, use1 = established(), usage_rekeys()
        # The monitor looks once a second, so how many re-keys one download sees depends on how fast the
        # machine is. Download until at least three have happened (at most six times); every download
        # must arrive intact, and each re-key must complete.
        intact, downloads, seconds = True, 0, 0.0
        for downloads in range(1, 7):
            before_download = usage_rekeys()
            r = D.sh(sys.executable, "-c", fetch, ns="ha", timeout=120)
            out = r.stdout.split()
            ok = r.returncode == 0 and len(out) == 3 and out[0] == str(len(blob)) and out[1] == want
            intact = intact and ok
            if not ok:
                print(f"    download {downloads}: rc={r.returncode} {r.stdout[-120:]} {r.stderr[-200:]}")
                break
            seconds += float(out[2])
            # Synchronize observed progress instead of letting all six transfers
            # finish while the controller is still sampling/re-baselining.
            CT.wait_for(lambda: usage_rekeys() > before_download, 5)
            CT.wait_for(lambda: established() - est1 >= usage_rekeys() - use1, 10)
            if usage_rekeys() - use1 >= 3:
                break
        check("every-download-arrives-intact-through-the-re-keys", intact)
        check("downloads-triggered-at-least-three-usage-rekeys", usage_rekeys() - use1 >= 3,
              f"{usage_rekeys() - use1} usage re-keys in {downloads} downloads")
        check("each-usage-rekey-completed", CT.wait_for(lambda: established() - est1 >= usage_rekeys() - use1, 10),
              f"{established() - est1} new establishments for {usage_rekeys() - use1} requests")
        print(f"    {downloads} x 48 MiB in {seconds:.1f} s, {usage_rekeys() - use1} usage re-keys")

        # 3. Small packets: records, not bytes. 6000 echo requests of 56 bytes is about 0.5 MB.
        CT.wait_for(lambda: True, 1)
        time.sleep(2)  # let the last exchange settle so the baseline is fresh
        use2 = usage_rekeys()
        D.sh("ping", "-q", "-c", "6000", "-i", "0.002", "-W", "1", "10.77.2.10", ns="ha", timeout=60)
        check("small-packet-burst-triggers-a-usage-rekey", CT.wait_for(lambda: usage_rekeys() > use2, 5),
              f"{usage_rekeys() - use2}")

        check("tunnel-still-works-afterwards", CT.wait_for(CT.ping_ok, 10), ca.text()[-300:])
    finally:
        for c in ctls:
            c.stop()
        for s in servers:
            s.terminate()
        CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"AY4_USAGE_REKEY {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
