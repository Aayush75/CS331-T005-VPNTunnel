#!/usr/bin/env python3
"""AY5 runtime evidence: ordinary data stays in the kernel, and lifetimes survive abuse.

1. No data through userspace. While 48 MiB crosses the tunnel the controllers use almost no CPU, own no
   TCP or UDP socket (only their Generic Netlink socket), and send and receive no exchange messages.
2. Churn. Round after round, under continuous traffic: a controller is killed without warning, or a
   gateway's `vpn0` is deleted from under a running controller and created again, and the tunnel must come
   back by itself each time (the controller's session loop, the kernel's lease and teardown).
3. Teardown under load. A bulk download runs while one gateway's whole network namespace is deleted.
4. The kernel log has no BUG, WARNING, lockdep, RCU-stall or lockup report from any of it. (The suite's
   "no leaked objects" check after the run covers what the module leaves behind.)

  VPT1_CHURN_ROUNDS=4 (default)   VPTCTL=/path/to/vptctl ay5_runtime.py
Run as root in a disposable VM with vpt1_foundation loaded.
"""
import hashlib
import os
import random
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_controller as CT  # noqa: E402  (controller helpers and the two-gateway configuration)

D = CT.D
ROUNDS = int(os.environ.get("VPT1_CHURN_ROUNDS", "4"))
FAULTS = re.compile(r"BUG:|WARNING:|Oops|Call Trace|RCU stall|soft lockup|hung task|lockdep|possible circular")
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def cpu_seconds(pid):
    fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def socket_inodes(pid):
    inodes = set()
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        m = re.match(r"socket:\[(\d+)\]", target)
        if m:
            inodes.add(m.group(1))
    return inodes


def data_socket_inodes(pid):
    """Inodes of this process's TCP/UDP/raw sockets, from its own network namespace."""
    found = set()
    for table in ("tcp", "tcp6", "udp", "udp6", "raw", "raw6"):
        try:
            for line in open(f"/proc/{pid}/net/{table}").read().splitlines()[1:]:
                found.add(line.split()[9])
        except OSError:
            pass
    return found


def kernel_faults(since_lines):
    out = subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines()
    return [ln for ln in out[since_lines:] if FAULTS.search(ln)]


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    if not CT.VPTCTL or not os.path.exists(CT.VPTCTL):
        sys.exit("set VPTCTL to the vptctl binary")
    log_start = len(subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines())
    r = CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="ay5_")
    os.chmod(tmp, 0o700)
    ctls, background = [], []
    rng = random.Random(20261104)
    try:
        def plumb(site, peer_net):
            D.sh("ip", "link", "add", "vpn0", "type", "vpt1_ay1", ns=site, check_rc=True)
            D.sh("ip", "link", "set", "vpn0", "up", ns=site, check_rc=True)
            D.sh("sysctl", "-q", "-w", "net.ipv4.conf.all.rp_filter=0", ns=site)
            D.sh("ip", "route", "add", peer_net, "dev", "vpn0", ns=site, check_rc=True)

        plumb("gwa", "10.77.2.0/24")
        plumb("gwb", "10.77.1.0/24")
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
        www = os.path.join(tmp, "www")
        os.makedirs(www)
        blob = os.urandom(48 * 1024 * 1024)
        open(os.path.join(www, "blob"), "wb").write(blob)
        want = hashlib.sha256(blob).hexdigest()
        background.append(subprocess.Popen(
            ["ip", "netns", "exec", D.NS["hb"], sys.executable, "-m", "http.server", "8080", "--bind", "10.77.2.10",
             "--directory", www], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.8)
        fetch = ("import hashlib,urllib.request;b=urllib.request.urlopen('http://10.77.2.10:8080/blob',timeout=90).read();"
                 "print(len(b),hashlib.sha256(b).hexdigest())")

        # ---- 1. No data through userspace (default timers: 90 s age, 300 MiB usage) ----
        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a.log"))
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b.log"))
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        check("tunnel-comes-up", CT.wait_for(CT.ping_ok, 20), ca.text()[-300:])
        time.sleep(1.5)
        counters0 = (CT.counters("gwa"), CT.counters("gwb"))
        cpu0 = (cpu_seconds(ca.proc.pid), cpu_seconds(cb.proc.pid))
        t0 = time.time()
        out = D.sh(sys.executable, "-c", fetch, ns="ha", timeout=120).stdout.split()
        took = time.time() - t0
        cpu1 = (cpu_seconds(ca.proc.pid), cpu_seconds(cb.proc.pid))
        counters1 = (CT.counters("gwa"), CT.counters("gwb"))
        check("48-mib-arrives-intact", out == [str(len(blob)), want], str(out)[:100])
        check("controllers-used-almost-no-cpu-while-the-kernel-moved-the-data",
              max(cpu1[0] - cpu0[0], cpu1[1] - cpu0[1]) < 0.5,
              f"{cpu1[0] - cpu0[0]:.2f} s and {cpu1[1] - cpu0[1]:.2f} s over {took:.1f} s")
        for name, c in (("a", ca), ("b", cb)):
            own, data = socket_inodes(c.proc.pid), data_socket_inodes(c.proc.pid)
            check(f"controller-{name}-owns-no-tcp-udp-or-raw-socket", own and not (own & data),
                  f"sockets {own}, data sockets among them {own & data}")
        moved = {k: counters1[0][k] - counters0[0][k] for k in ("TX_PACKETS", "RX_PACKETS")}
        check("the-kernel-carried-the-packets", moved["RX_PACKETS"] > 10000, str(moved))
        for i, name in enumerate(("a", "b")):
            check(f"no-exchange-messages-during-the-transfer-{name}",
                  all(counters1[i][k] == counters0[i][k] for k in ("TX_EXCHANGE", "RX_EXCHANGE")),
                  f"{counters0[i]} -> {counters1[i]}")
        print(f"    48 MiB in {took:.1f} s; controller CPU {cpu1[0] - cpu0[0]:.2f} s and {cpu1[1] - cpu0[1]:.2f} s")
        ca.stop()
        cb.stop()

        # ---- 2. Churn: abuse the controllers and the devices under traffic, with a 3 s re-key ----
        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a2.log"), rekey=3)
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b2.log"))
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        stop = threading.Event()
        loads = {"ok": 0, "cut": 0, "corrupt": 0}

        def load():
            while not stop.is_set():
                r = D.sh(sys.executable, "-c", fetch, ns="ha", timeout=150)
                got = r.stdout.split()
                if r.returncode == 0 and got == [str(len(blob)), want]:
                    loads["ok"] += 1
                elif r.returncode == 0 and got:
                    loads["corrupt"] += 1  # it finished with the wrong bytes: never acceptable
                elif not stop.is_set():
                    loads["cut"] += 1  # cut by the abuse (TCP gives up or times out): expected

        loader = threading.Thread(target=load, daemon=True)
        loader.start()
        background.append(subprocess.Popen(["ip", "netns", "exec", D.NS["ha"], "ping", "-q", "-i", "0.05", "-W", "1",
                                            "10.77.2.10"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        actions = ["kill-initiator", "kill-responder", "delete-vpn0-initiator", "delete-vpn0-responder"]
        for n in range(ROUNDS):
            check(f"round-{n + 1}-tunnel-up", CT.wait_for(CT.ping_ok, 45), "before the abuse")
            time.sleep(rng.uniform(0.5, 2.5))
            action = actions[n % len(actions)]
            ctl, site, net = (ca, "gwa", "10.77.2.0/24") if "initiator" in action else (cb, "gwb", "10.77.1.0/24")
            if action.startswith("kill"):
                ctl.proc.send_signal(signal.SIGKILL)
                ctl.proc.wait()
                time.sleep(rng.uniform(1.0, 6.5))  # sometimes before and sometimes after the 5 s lease runs out
                ctl.start()
            else:
                D.sh("ip", "link", "del", "vpn0", ns=site)
                time.sleep(rng.uniform(0.5, 3.0))
                plumb(site, net)
            check(f"round-{n + 1}-{action}-recovers", CT.wait_for(CT.ping_ok, 60, step=0.5),
                  f"after {action}: {ca.text()[-200:]} {cb.text()[-200:]}")
        stop.set()
        loader.join(timeout=150)
        print(f"    {ROUNDS} rounds; downloads whole: {loads['ok']}, cut by the abuse: {loads['cut']}, corrupt: {loads['corrupt']}")
        check("no-download-ever-arrived-corrupted", loads["corrupt"] == 0, str(loads))
        # A download started after all the abuse is intact: the tunnel really recovered.
        check("tunnel-up-after-the-last-round", CT.wait_for(CT.ping_ok, 45))
        again = D.sh(sys.executable, "-c", fetch, ns="ha", timeout=120).stdout.split()
        check("a-fresh-download-after-the-abuse-is-intact", again == [str(len(blob)), want], str(again)[:100])

        # ---- 3. Delete a whole gateway namespace while a download is running ----
        def doomed():
            try:
                D.sh(sys.executable, "-c", fetch, ns="ha", timeout=60)
            except subprocess.TimeoutExpired:
                pass  # the download is cut when the gateway disappears; that is the point

        victim = threading.Thread(target=doomed, daemon=True)
        victim.start()
        time.sleep(2.0)
        ca.stop()
        CT.run("ip", "netns", "del", D.NS["gwa"])
        time.sleep(2.0)
        check("the-namespace-is-gone", not os.path.exists(f"/var/run/netns/{D.NS['gwa']}"))
        victim.join(timeout=80)

        # ---- 4. The kernel log ----
        time.sleep(1.0)
        found = kernel_faults(log_start)
        check("no-kernel-bug-warning-lockdep-or-stall-in-the-log", not found, "\n".join(found[:6]))
    finally:
        stop_all = [p for p in background]
        for p in stop_all:
            p.terminate()
        for c in ctls:
            c.stop()
        CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"AY5_RUNTIME {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
