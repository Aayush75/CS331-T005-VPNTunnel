#!/usr/bin/env python3
"""Live reconfiguration (RJ4): a changed configuration file is applied to a running tunnel.

Two real `vptctl run` processes bring the tunnel up. Then gateway A's configuration file is
edited while traffic runs, and the controller must follow the kernel's own classification:

  - a policy-only change (one more rule, then the rule removed again) is committed as the next
    revision with no packet lost, no new exchange and no new key; the new rule is enforced at once,
    and a revoked rule stops being enforced at once,
  - a change the controller cannot or must not apply (garbage, a different gateway key) is
    rejected, logged once, and leaves the running configuration and the tunnel untouched,
  - a change to the peer itself (one more owned prefix) makes the kernel replace the peer, and the
    controller starts a fresh exchange at once and the tunnel comes back.

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl rj4_reload.py
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
REPO = CT.REPO
passed = failed = 0
EXTRA_RULE = """
[[rule]]
id = 6
peer = "peer"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "9999"
"""


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def write_atomic(path, text):
    """What an editor or a config-management tool does: write beside, then rename over."""
    tmp = path + ".new"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def http(port, timeout="1.5"):
    r = D.sh(sys.executable, os.path.join(REPO, "tools/oracles/http_client.py"), f"http://10.77.2.10:{port}",
             "--expect-server", "host-b", "--expect-client-ip", "10.77.1.10", "--timeout", timeout,
             ns="ha", timeout=30)
    return r.returncode == 0


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    if not CT.VPTCTL or not os.path.exists(CT.VPTCTL):
        sys.exit("set VPTCTL to the vptctl binary")
    r = CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="rj4reload_")
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
        for who in ("a", "b", "other"):
            keys[who] = os.path.join(tmp, f"{who}.key")
            out = CT.run(CT.VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a, cfg_b = os.path.join(tmp, "a.toml"), os.path.join(tmp, "b.toml")
        base_a = CT.toml("a", pubs["a"], pubs["b"], "172.31.250.12", "10.77.2.0/24", "initiator",
                         "veth_gwa_lan", "veth_gwa_out", "10.77.1.1/24", "172.31.250.11/24", keys["a"])
        base_b = CT.toml("b", pubs["b"], pubs["a"], "172.31.250.11", "10.77.1.0/24", "responder",
                         "veth_gwb_lan", "veth_gwb_out", "10.77.2.1/24", "172.31.250.12/24", keys["b"])
        write_atomic(cfg_a, base_a)
        write_atomic(cfg_b, base_b)
        for port in (8080, 9999):
            servers.append(subprocess.Popen(
                ["ip", "netns", "exec", D.NS["hb"], sys.executable, os.path.join(REPO, "tools/oracles/http_server.py"),
                 "--host", "10.77.2.10", "--port", str(port), "--server-id", "host-b"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.6)

        ca = CT.Controller("gwa", cfg_a, os.path.join(tmp, "a.log"), rekey=3600)
        cb = CT.Controller("gwb", cfg_b, os.path.join(tmp, "b.log"))
        ctls += [ca, cb]
        cb.start()
        time.sleep(0.3)
        ca.start()
        check("tunnel-comes-up", CT.wait_for(CT.ping_ok, 20), ca.text()[-300:])
        check("the-allowed-port-works", http(8080))
        check("a-port-no-rule-allows-is-denied-before-the-change", not http(9999))

        def inits():
            return ca.text().count("sent INIT")

        def log_has(text):
            return text in ca.text()

        # 1. Policy-only change while traffic runs: one more rule. Both gateways enforce the policy (the
        # sender before the tunnel, the receiver after it), so the port opens only when both have it.
        # Nothing may be lost or re-keyed.
        inits0, tx0 = inits(), CT.counters("gwa")["TX_EXCHANGE"]
        ping = subprocess.Popen(["ip", "netns", "exec", D.NS["ha"], "ping", "-c", "200", "-i", "0.05", "-W", "1",
                                 "10.77.2.10"], stdout=subprocess.PIPE, text=True)
        time.sleep(1.5)
        write_atomic(cfg_a, base_a + EXTRA_RULE)
        check("rule-added-is-reloaded-as-revision-2",
              CT.wait_for(lambda: log_has("reloaded as revision 2"), 8), ca.text()[-400:])
        check("one-gateway-alone-does-not-open-the-port", not http(9999))
        write_atomic(cfg_b, base_b + EXTRA_RULE)
        check("the-responder-reloads-its-own-file",
              CT.wait_for(lambda: "reloaded as revision 2" in cb.text(), 8), cb.text()[-400:])
        m = re.search(r"(\d+) packets transmitted, (\d+) received", ping.communicate(timeout=60)[0])
        lost = int(m[1]) - int(m[2]) if m else 999
        check("no-packet-lost-across-a-policy-only-reload", lost == 0, f"{lost} lost")
        for who, ctl in (("initiator", ca), ("responder", cb)):
            check(f"policy-only-reload-kept-the-{who}-peer",
                  "kept [1], replaced [], added [], removed []" in ctl.text(), ctl.text()[-300:])
        check("policy-only-reload-started-no-exchange", inits() == inits0 and
              CT.counters("gwa")["TX_EXCHANGE"] == tx0, f"{inits0}->{inits()}")
        check("the-new-rule-is-enforced-once-both-have-it", http(9999))
        check("the-old-rule-still-works", http(8080))

        # 2. The same change reversed: a revoked rule stops being enforced at once, on the first
        # gateway that revokes it.
        write_atomic(cfg_a, base_a)
        check("rule-removed-is-reloaded-as-revision-3",
              CT.wait_for(lambda: log_has("reloaded as revision 3"), 8), ca.text()[-400:])
        check("the-revoked-rule-is-denied-at-once", not http(9999))
        write_atomic(cfg_b, base_b)
        check("the-responder-revokes-too",
              CT.wait_for(lambda: "reloaded as revision 3" in cb.text(), 8), cb.text()[-400:])
        check("traffic-the-rules-still-allow-works", http(8080) and CT.ping_ok())
        check("revoking-started-no-exchange", inits() == inits0, f"{inits0}->{inits()}")

        # 3. Changes that must be refused, each logged once, leaving everything as it was.
        def rejected():
            return ca.text().count("configuration change rejected")

        n = rejected()
        write_atomic(cfg_a, "this is [not toml\n")
        check("garbage-is-rejected", CT.wait_for(lambda: rejected() == n + 1, 8), ca.text()[-300:])
        check("rejection-names-the-running-revision", "still running revision 3" in ca.text())
        time.sleep(3.5)
        check("a-rejected-file-is-not-retried-or-logged-again", rejected() == n + 1, f"{rejected() - n}")
        check("tunnel-unaffected-by-the-garbage", http(8080) and not http(9999))

        write_atomic(cfg_a, base_a.replace(keys["a"], keys["other"]))
        check("a-different-gateway-key-is-refused-at-runtime",
              CT.wait_for(lambda: log_has("own key changed"), 8), ca.text()[-300:])
        check("tunnel-unaffected-by-the-refused-key", http(8080))

        write_atomic(cfg_a, base_a)
        n_reload = ca.text().count("reloaded as revision")
        time.sleep(3.5)
        check("restoring-the-running-file-changes-nothing",
              ca.text().count("reloaded as revision") == n_reload and rejected() == n + 2,
              f"{n_reload} reloads, {rejected() - n} rejections")

        # 4. A change to the peer itself (one more owned prefix): the kernel replaces the peer, so the
        # controller starts a fresh exchange at once and the tunnel comes back without help.
        est0 = ca.text().count("established (initiator)")
        write_atomic(cfg_a, base_a.replace('prefixes = ["10.77.2.0/24"]',
                                           'prefixes = ["10.77.2.0/24", "10.77.5.0/24"]'))
        check("a-peer-change-is-reloaded-as-revision-4",
              CT.wait_for(lambda: log_has("reloaded as revision 4"), 8), ca.text()[-400:])
        check("the-peer-was-replaced", "replaced [1]" in ca.text(), ca.text()[-300:])
        check("a-fresh-exchange-started", CT.wait_for(lambda: inits() == inits0 + 1, 5), f"{inits0}->{inits()}")
        check("the-tunnel-comes-back", CT.wait_for(CT.ping_ok, 20), ca.text()[-300:])
        check("the-initiator-established-again",
              CT.wait_for(lambda: ca.text().count("established (initiator)") == est0 + 1, 10))
        check("traffic-works-after-the-peer-change", http(8080))

        faults = D.sh("bash", "-c", "dmesg | grep -E 'BUG:|WARNING:|Oops|KASAN|UBSAN|refcount_t|lockdep' || true")
        check("no-kernel-fault-markers", not faults.stdout.strip(), faults.stdout[-300:])
    finally:
        for c in ctls:
            c.stop()
        for s in servers:
            s.terminate()
        CT.run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"RJ4_RELOAD {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
