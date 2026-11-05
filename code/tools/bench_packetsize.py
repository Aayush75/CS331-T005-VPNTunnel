#!/usr/bin/env python3
"""Packet-size sweep for VPTunnel, over the namespace tunnel.

Brings the VPN up on the N1 topology (as the demo does) and sends UDP at a fixed rate with
a range of datagram sizes, reporting delivered throughput and loss for each size. This shows
the effect of the tunnel's fixed 72-byte per-packet overhead and the vpn0 inner MTU of 1428:
small datagrams pay a high relative overhead, and inner datagrams whose encapsulated size
would exceed the underlay MTU are dropped by design (fragments do not cross).

Must run as root on a DISPOSABLE Linux 6.18.y machine (the reference VM). Needs iperf3.

  sudo VPT1_DISPOSABLE_VM=yes VPTCTL=.../vptctl python3 tools/bench_packetsize.py
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
NETNS = os.path.join(REPO, "tests", "netns")
sys.path.insert(0, NETNS)
import ay2_datapath as D  # noqa: E402

# Reuse the tunnel bring-up from the throughput benchmark.
sys.path.insert(0, HERE)
import bench_netns as B  # noqa: E402

HA, HB = B.HA, B.HB
SIZES = [64, 128, 256, 512, 768, 1024, 1200, 1300, 1400, 1428]


def say(*a):
    print(*a, flush=True)


def udp_size_point(size, seconds, rate_mbit):
    srv = subprocess.Popen(["ip", "netns", "exec", f"vpt_{D.RUN}_hb", "iperf3", "-s", "-1", "-B", HB],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    try:
        r = B.host("ha", "iperf3", "-c", HB, "-B", HA, "-u", "-l", str(size),
                   "-b", f"{rate_mbit}M", "-t", str(seconds), "-J", timeout=seconds + 20)
        data = json.loads(r.stdout) if r.stdout.strip().startswith("{") else {}
        end = data.get("end", {}).get("sum", {})
        if not end:
            return None
        return dict(size=size, mbps=round(end.get("bits_per_second", 0) / 1e6, 1),
                    loss_pct=round(end.get("lost_percent", 0), 2),
                    pps=round(end.get("packets", 0) / max(seconds, 1)))
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            srv.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--rate", type=int, default=200, help="UDP offered load per size, Mbit/s")
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()
    if os.geteuid() != 0 or os.environ.get("VPT1_DISPOSABLE_VM") != "yes":
        ap.error("run as root with VPT1_DISPOSABLE_VM=yes on a disposable VM")
    B.need("iperf3")
    if not B.VPTCTL or not os.path.exists(B.VPTCTL):
        ap.error("set VPTCTL to the vptctl binary")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    outdir = args.output or Path(REPO) / "results" / f"{stamp}-packet"
    outdir.mkdir(parents=True, exist_ok=True)
    report = dict(kind="packet-size-sweep", kernel=os.uname().release, machine=os.uname().machine,
                  seconds=args.seconds, rate_mbit=args.rate, inner_mtu=1428, overhead_bytes=72,
                  started=stamp, points=[])

    r = B.run("bash", os.path.join(NETNS, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="pktbench_")
    os.chmod(tmp, 0o700)
    pis = []
    try:
        B.host_addrs()
        keys, pubs = {}, {}
        for who in ("a", "b"):
            keys[who] = os.path.join(tmp, f"{who}.key")
            out = B.run(B.VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a = B.make_config("demo-pi-a.toml", "a", tmp, keys["a"], pubs["b"])
        cfg_b = B.make_config("demo-pi-b.toml", "b", tmp, keys["b"], pubs["a"])
        pi_a, pi_b = B.Pi("gwa", cfg_a, tmp), B.Pi("gwb", cfg_b, tmp)
        pis += [pi_a, pi_b]
        say("==> bringing the VPN up")
        pi_b.script("up")
        pi_a.script("up", "--wait", "20")
        if B.host("ha", "ping", "-c", "2", "-W", "2", HB).returncode != 0:
            sys.exit("tunnel did not come up; aborting")
        for size in SIZES:
            say(f"  UDP datagram {size} bytes")
            pt = udp_size_point(size, args.seconds, args.rate)
            if pt:
                report["points"].append(pt)
    finally:
        for p in pis:
            p.script("down")
        B.run("bash", os.path.join(NETNS, "n1_fixture.sh"), "teardown", D.RUN)

    (outdir / "packet.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# VPTunnel packet-size sweep", "",
             f"- Kernel: `{report['kernel']}` ({report['machine']}); UDP {report['rate_mbit']} Mbit/s "
             f"offered, {report['seconds']} s per size", f"- vpn0 inner MTU {report['inner_mtu']}; "
             f"tunnel overhead {report['overhead_bytes']} bytes/packet", "",
             "| datagram bytes | delivered Mbit/s | loss % | packets/s |", "|---|---|---|---|"]
    for p in report["points"]:
        lines.append(f"| {p['size']} | {p['mbps']} | {p['loss_pct']} | {p['pps']} |")
    lines += ["", "Small datagrams pay the fixed 72-byte overhead on every packet, so delivered "
              "goodput is lower and the packet rate is the limit. Near the 1428-byte inner MTU the "
              "overhead is amortised. Inner datagrams that would exceed the underlay MTU once "
              "encapsulated are dropped by design (the tunnel does not fragment)."]
    (outdir / "summary.md").write_text("\n".join(lines) + "\n")
    say(f"==> results in {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
