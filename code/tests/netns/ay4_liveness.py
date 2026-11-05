#!/usr/bin/env python3
"""Kernel liveness (PING / PONG) end to end, on a real kernel.

Two gateways on the N1 fixture, keyed through BEGIN_ATTEMPT and STAGE_GENERATION, no controller process:
the test is the controller, so nothing but the kernel's own control records can keep a tunnel alive. Proves:

  * an idle tunnel exchanges PING and PONG records on its own and stays open;
  * a ONE-WAY failure (gateway B's datagrams are dropped on the underlay, A's still arrive, so B keeps
    receiving A's PINGs and pings) closes BOTH sides after the 15 s without an authenticated round trip,
    each counted once: inbound traffic does not hide it;
  * a closed tunnel carries no application traffic and sends no more PINGs;
  * the underlay coming back does not reopen it (a late answer cannot);
  * a new session (BEGIN_ATTEMPT and STAGE_GENERATION with new keys) opens it again, and PINGs resume.

Run as root inside a disposable VM with vpt1_foundation loaded:  ay4_liveness.py
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_datapath as D  # noqa: E402
import ay2_genl as G  # noqa: E402

# Counter ids appended after RX_ECN_ANOMALY (see vpt1_gateway.h).
D.C.update(TX_PING=36, RX_PING=37, TX_PONG=38, RX_PONG=39, RX_CONTROL_DROPPED=40, LIVENESS_LOST=41)
check = D.check
sh = D.sh
K3, K4 = bytes(range(60, 92)), bytes(range(160, 192))


def restage(gw):
    """A new session on an already configured peer: a new attempt, then new keys."""
    inc = gw.incarnation()
    err, reply = gw.cmd("BEGIN_ATTEMPT", G.a("PEER_SLOT", G.u16(1)), G.a("PEER_INCARNATION", inc))
    check(f"{gw.site}-begin-new-attempt", err == 0, f"err={err}")
    gw.attempt = reply[G.A["ATTEMPT_ID"]][0]
    gw.local_rx = struct.unpack("=Q", reply[G.A["LOCAL_RX"]][0])[0]
    gw.inc = inc


def stage(gw, tx, rx, remote_rx):
    err = gw.cmd("STAGE_GENERATION", G.a("PEER_SLOT", G.u16(1)), G.a("PEER_INCARNATION", gw.inc),
                 G.a("ATTEMPT_ID", gw.attempt), G.a("HANDSHAKE_HASH", b"N" * 32),
                 G.a("LOCAL_RX", G.u64(gw.local_rx)), G.a("REMOTE_RX", G.u64(remote_rx)),
                 G.a("TX_KEY", tx), G.a("RX_KEY", rx), G.a("MODE", b"\0"))[0]
    check(f"{gw.site}-stage-new-session", err == 0, f"err={err}")


def main():
    if os.geteuid() != 0 or os.environ.get("VPT1_DISPOSABLE_VM") != "yes":
        sys.exit("run as root in a disposable VM with VPT1_DISPOSABLE_VM=yes")
    r = subprocess.run(["bash", os.path.join(HERE, "n1_fixture.sh"), "setup", D.RUN],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="ay4live_")
    procs = []
    try:
        for site in ("gwa", "gwb"):
            sh("ip", "link", "add", "vpn0", "type", "vpt1_ay1", ns=site, check_rc=True)
            sh("ip", "link", "set", "vpn0", "up", ns=site, check_rc=True)
            sh("sysctl", "-q", "-w", "net.ipv4.conf.all.rp_filter=0", ns=site)
        sh("ip", "route", "add", "10.77.2.0/24", "dev", "vpn0", ns="gwa", check_rc=True)
        sh("ip", "route", "add", "10.77.1.0/24", "dev", "vpn0", ns="gwb", check_rc=True)
        ga, gb = D.Gateway("gwa", D.ID_A), D.Gateway("gwb", D.ID_B)
        rules_a = [(1, 1, 1, 0, "10.77.1.10", "10.77.2.10")]
        rules_b = [(1, 2, 1, 0, "10.77.1.10", "10.77.2.10")]
        D.prepare(ga, D.snapshot(ga.lan_if, ga.und_if, "10.77.1.0", "10.77.1.1", "172.31.250.11",
                                 "172.31.250.12", "10.77.2.0", rules_a, 0x10, 0x20))
        D.prepare(gb, D.snapshot(gb.lan_if, gb.und_if, "10.77.2.0", "10.77.2.1", "172.31.250.12",
                                 "172.31.250.11", "10.77.1.0", rules_b, 0x20, 0x10))
        D.finish(ga, D.K1, D.K2, gb.local_rx)
        D.finish(gb, D.K2, D.K1, ga.local_rx)
        D.wait_active(ga, gb)
        time.sleep(0.5)

        def ping(count=2, wait=1):
            return sh("ping", "-c", str(count), "-W", str(wait), "10.77.2.10", ns="ha", timeout=30)

        # 1. Idle and healthy: the kernels talk to each other on their own.
        check("traffic-before", ping().returncode == 0)
        time.sleep(8)
        a, b = ga.counters(), gb.counters()
        for name, c in (("a", a), ("b", b)):
            check(f"{name}-sent-pings", c["TX_PING"] >= 1, str(c["TX_PING"]))
            check(f"{name}-answered-pings", c["TX_PONG"] >= 1 and c["RX_PING"] >= 1,
                  f"tx_pong={c['TX_PONG']} rx_ping={c['RX_PING']}")
            check(f"{name}-got-answers", c["RX_PONG"] >= 1, str(c["RX_PONG"]))
            check(f"{name}-not-closed", c["LIVENESS_LOST"] == 0)
        check("traffic-still-works", ping().returncode == 0)
        a_pings = a["TX_PING"]

        # 2. A one-way failure: B's datagrams stop reaching A. A keeps sending PINGs and application
        # traffic to B, so B receives constantly, but nothing it sends comes back answered.
        sh("nft", "add", "table", "inet", "livetest", ns="gwb", check_rc=True)
        sh("nft", "add", "chain", "inet", "livetest", "out", "{ type filter hook output priority 0 ; }",
           ns="gwb", check_rc=True)
        sh("nft", "add", "rule", "inet", "livetest", "out", "udp", "dport", "51888", "drop", ns="gwb", check_rc=True)
        flood = subprocess.Popen(["ip", "netns", "exec", D.NS["ha"], "ping", "-i", "0.5", "-W", "1",
                                  "10.77.2.10"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(flood)
        t0 = time.time()
        lost = None
        while time.time() - t0 < 30:
            a, b = ga.counters(), gb.counters()
            if a["LIVENESS_LOST"] and b["LIVENESS_LOST"]:
                lost = time.time() - t0
                break
            time.sleep(0.5)
        check("both-sides-closed", lost is not None, f"a={a['LIVENESS_LOST']} b={b['LIVENESS_LOST']}")
        if lost is not None:
            # No round trip for 15 s, detected by a once-a-second tick; the last good PONG was up to 5 s
            # before the failure began, so closing lands between 10 s and 21 s after it.
            check("closed-after-about-15-s", 9 <= lost <= 22, f"{lost:.1f} s")
        check("b-was-still-receiving-when-it-closed", b["RX_PING"] > 0 and b["RX_PACKETS"] > 0)
        time.sleep(3)
        a2, b2 = ga.counters(), gb.counters()
        check("counted-once", a2["LIVENESS_LOST"] == 1 and b2["LIVENESS_LOST"] == 1,
              f"a={a2['LIVENESS_LOST']} b={b2['LIVENESS_LOST']}")
        flood.terminate()
        flood.wait()

        # 3. Closed: no application traffic and no more PINGs, even once the underlay is healthy.
        sh("nft", "delete", "table", "inet", "livetest", ns="gwb", check_rc=True)
        pings_closed = (ga.counters()["TX_PING"], gb.counters()["TX_PING"])
        check("closed-tunnel-carries-nothing", ping(count=3, wait=1).returncode != 0)
        time.sleep(7)
        now_a, now_b = ga.counters(), gb.counters()
        check("closed-peers-send-no-pings", (now_a["TX_PING"], now_b["TX_PING"]) == pings_closed,
              f"{pings_closed} -> {(now_a['TX_PING'], now_b['TX_PING'])}")
        check("late-recovery-does-not-reopen", ping(count=2, wait=1).returncode != 0)

        # 4. A new session opens it again; nothing else does.
        activated = {g.site: g.counters()["ACTIVATED"] for g in (ga, gb)}
        restage(ga)
        restage(gb)
        stage(ga, K3, K4, gb.local_rx)
        stage(gb, K4, K3, ga.local_rx)
        D.wait_active(ga, gb, before=activated)
        time.sleep(0.5)
        check("new-session-carries-traffic", ping(count=3, wait=2).returncode == 0)
        time.sleep(7)
        end_a, end_b = ga.counters(), gb.counters()
        check("pings-resume", end_a["TX_PING"] > now_a["TX_PING"] and end_b["TX_PING"] > now_b["TX_PING"])
        check("answers-resume", end_a["RX_PONG"] > now_a["RX_PONG"] and end_b["RX_PONG"] > now_b["RX_PONG"])
        check("reopened-and-not-closed-again", end_a["LIVENESS_LOST"] == 1 and end_b["LIVENESS_LOST"] == 1)
        check("traffic-after-new-session", ping().returncode == 0)
        print(f"    {a_pings} PINGs in the first 9 s; closed {lost:.1f} s into the one-way failure"
              if lost is not None else "    (no closure measured)")
    finally:
        for p in procs:
            p.terminate()
        subprocess.run(["bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN], capture_output=True)
    print(f"AY4_LIVENESS {'PASS' if not D.failed else 'FAIL'} passed={D.passed} failed={D.failed}")
    sys.exit(1 if D.failed else 0)


if __name__ == "__main__":
    main()
