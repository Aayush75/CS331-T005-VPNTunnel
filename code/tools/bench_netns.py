#!/usr/bin/env python3
"""Namespace throughput / latency benchmark for VPTunnel.

Stands up the N1 topology (client host-a, gateway A, underlay, gateway B, service host-b)
and brings both gateways up with the real controller (vpt-gateway.sh + the kernel module),
exactly as the demo does, then measures the application path host-a -> host-b over the VPN.
It then measures a DIRECT baseline: the same two host namespaces reachable through the
underlay with ordinary IPv4 forwarding and no tunnel, so the comparison isolates the
tunnel's cost rather than comparing two unrelated paths.

Per configuration: TCP goodput (iperf3, both directions, several runs) and ping RTT, plus
a UDP rate/loss point. Results as JSON + a Markdown summary under results/<timestamp>-netns/.

Must run as root on a DISPOSABLE Linux 6.18.y machine (the reference VM). Needs iperf3.

  sudo VPT1_DISPOSABLE_VM=yes VPTCTL=.../vptctl python3 tools/bench_netns.py --runs 5 --seconds 15
"""
import argparse
import json
import os
import re
import statistics
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
import ay2_datapath as D  # noqa: E402  (RUN id, NS map, sh helper)

GATEWAY_SH = os.path.join(REPO, "infra", "gateway", "vpt-gateway.sh")
HA, HB = "10.77.1.57", "10.77.2.88"
VPTCTL = os.environ.get("VPTCTL", "")


def say(*a):
    print(*a, flush=True)


def run(*argv, **kw):
    return subprocess.run(list(argv), capture_output=True, text=True, **kw)


def host(ns_short, *argv, timeout=120):
    return run("ip", "netns", "exec", f"vpt_{D.RUN}_{ns_short}", *argv, timeout=timeout)


def need(binary):
    if not any(os.access(os.path.join(p, binary), os.X_OK)
               for p in os.environ.get("PATH", "").split(os.pathsep)):
        sys.exit(f"benchmark needs '{binary}' installed (see docs/benchmarking.md)")


# ---- measurement ----------------------------------------------------------------

def ping_rtt(src, dst, count=50):
    r = host(src, "ping", "-c", str(count), "-i", "0.05", "-W", "1", dst)
    m = re.search(r"=\s*([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)\s*ms", r.stdout)
    return None if not m else dict(min=float(m.group(1)), avg=float(m.group(2)),
                                   max=float(m.group(3)), mdev=float(m.group(4)))


def iperf_tcp(src, dst_short, dst_ip, src_ip, seconds, reverse):
    srv = subprocess.Popen(["ip", "netns", "exec", f"vpt_{D.RUN}_{dst_short}",
                            "iperf3", "-s", "-1", "-B", dst_ip],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    try:
        argv = ["iperf3", "-c", dst_ip, "-B", src_ip, "-t", str(seconds), "-O", "2", "-J"]
        if reverse:
            argv.append("-R")
        r = host(src, *argv, timeout=seconds + 20)
        data = json.loads(r.stdout) if r.stdout.strip().startswith("{") else {}
        end = data.get("end", {}).get("sum_received", {})
        return end.get("bits_per_second", 0) / 1e6 if end else None
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            srv.kill()


def iperf_udp(src, dst_short, dst_ip, src_ip, seconds, rate_mbit):
    srv = subprocess.Popen(["ip", "netns", "exec", f"vpt_{D.RUN}_{dst_short}",
                            "iperf3", "-s", "-1", "-B", dst_ip],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    try:
        r = host(src, "iperf3", "-c", dst_ip, "-B", src_ip, "-u", "-b", f"{rate_mbit}M",
                 "-t", str(seconds), "-J", timeout=seconds + 20)
        data = json.loads(r.stdout) if r.stdout.strip().startswith("{") else {}
        end = data.get("end", {}).get("sum", {})
        return dict(mbps=round(end.get("bits_per_second", 0) / 1e6, 1),
                    loss_pct=round(end.get("lost_percent", 0), 2)) if end else None
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            srv.kill()


def agg(xs):
    if not xs:
        return None
    return dict(mean=round(statistics.mean(xs), 1),
                stdev=round(statistics.pstdev(xs), 1) if len(xs) > 1 else 0.0,
                runs=[round(x, 1) for x in xs])


def measure(label, runs, seconds, udp_rate):
    say(f"  [{label}] ping RTT")
    rtt = ping_rtt("ha", HB)
    say(f"  [{label}] TCP x{runs} ({seconds}s), both directions")
    fwd = [v for v in (iperf_tcp("ha", "hb", HB, HA, seconds, False) for _ in range(runs)) if v]
    rev = [v for v in (iperf_tcp("ha", "hb", HB, HA, seconds, True) for _ in range(runs)) if v]
    say(f"  [{label}] UDP at {udp_rate} Mbit/s")
    udp = iperf_udp("ha", "hb", HB, HA, seconds, udp_rate)
    return dict(label=label, ping_rtt_ms=rtt, tcp_fwd_mbps=agg(fwd), tcp_rev_mbps=agg(rev), udp=udp)


# ---- topology bring-up (reuses the demo's real machinery) -----------------------

MODULE = os.environ.get("VPT1_MODULE") or os.path.join(REPO, "kernel", "vpt1_foundation.ko")


class Pi:
    def __init__(self, site, config, tmp):
        self.site, self.config = site, config
        self.env = {"VPTCTL": VPTCTL, "VPT1_MODULE": MODULE,
                    "VPT_RUN_DIR": os.path.join(tmp, f"run-{site}"),
                    "PATH": os.environ["PATH"]}

    def script(self, action, *extra):
        return run("ip", "netns", "exec", f"vpt_{D.RUN}_{self.site}", "env",
                   *[f"{k}={v}" for k, v in self.env.items()],
                   "bash", GATEWAY_SH, action, self.config, *extra, timeout=60)


def make_config(template, site, tmp, own_key, peer_pub):
    text = open(os.path.join(REPO, "infra/gateway", template)).read()
    text = re.sub(r'private_key_file = ".*"', f'private_key_file = "{own_key}"', text)
    text = text.replace('"wlan0"', f'"veth_gw{site}_lan"').replace('"eth0"', f'"veth_gw{site}_out"')
    text = re.sub(r'public_key = "[0-9a-f]{64}"', f'public_key = "{peer_pub}"', text)
    path = os.path.join(tmp, template)
    open(path, "w").write(text)
    return path


def host_addrs():
    """Give host-a / host-b the addresses the services bind to, and default routes via the gateway."""
    for h, gw in (("ha", "10.77.1.1"), ("hb", "10.77.2.1")):
        host(h, "ip", "route", "replace", "default", "via", gw)
    host("ha", "ip", "addr", "add", f"{HA}/24", "dev", "veth_ha")
    host("hb", "ip", "addr", "add", f"{HB}/24", "dev", "veth_hb")


def baseline_setup():
    """Direct path: route host-a <-> host-b through the two gateway namespaces WITHOUT a tunnel,
    using plain IPv4 forwarding across the underlay. Same host namespaces, no VPN.

    The tunnel teardown (vpt-gateway.sh down) leaves a gateway firewall that forwards nothing and
    removes vpn0; this restores plain forwarding so the identical two hosts have a direct path."""
    for gw in ("gwa", "gwb"):
        host(gw, "sysctl", "-q", "-w", "net.ipv4.ip_forward=1")
        # Remove the "forward nothing" table that `down` leaves, so plain forwarding is allowed.
        host(gw, "nft", "delete", "table", "inet", "vpt_gateway")
    # Each gateway reaches the far LAN via the other gateway's underlay address.
    host("gwa", "ip", "route", "replace", "10.77.2.0/24", "via", "172.31.250.12")
    host("gwb", "ip", "route", "replace", "10.77.1.0/24", "via", "172.31.250.11")
    # The hosts' default route still points at their own gateway (set in host_addrs); confirm it.
    for h, gw in (("ha", "10.77.1.1"), ("hb", "10.77.2.1")):
        host(h, "ip", "route", "replace", "default", "via", gw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--udp-rate", type=int, default=100, help="UDP offered load, Mbit/s")
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()

    if os.geteuid() != 0 or os.environ.get("VPT1_DISPOSABLE_VM") != "yes":
        ap.error("run as root with VPT1_DISPOSABLE_VM=yes on a disposable VM")
    need("iperf3")
    if not VPTCTL or not os.path.exists(VPTCTL):
        ap.error("set VPTCTL to the vptctl binary (control/target/release/vptctl)")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    outdir = args.output or Path(REPO) / "results" / f"{stamp}-netns"
    outdir.mkdir(parents=True, exist_ok=True)
    report = dict(kind="netns-benchmark", kernel=os.uname().release, machine=os.uname().machine,
                  runs=args.runs, seconds=args.seconds, udp_rate_mbit=args.udp_rate,
                  started=stamp, configs=[])

    r = run("bash", os.path.join(NETNS, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="bench_")
    os.chmod(tmp, 0o700)
    pis = []
    try:
        host_addrs()

        # --- tunnel configuration ---
        keys, pubs = {}, {}
        for who in ("a", "b"):
            keys[who] = os.path.join(tmp, f"{who}.key")
            out = run(VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a = make_config("demo-pi-a.toml", "a", tmp, keys["a"], pubs["b"])
        cfg_b = make_config("demo-pi-b.toml", "b", tmp, keys["b"], pubs["a"])
        pi_a, pi_b = Pi("gwa", cfg_a, tmp), Pi("gwb", cfg_b, tmp)
        pis += [pi_a, pi_b]
        say("==> bringing the VPN up (start B, then A)")
        if pi_b.script("up").returncode or pi_a.script("up", "--wait", "20").returncode:
            say("    gateway bring-up failed; see controller logs")
        # Confirm the tunnel is actually carrying traffic before measuring.
        if host("ha", "ping", "-c", "2", "-W", "2", HB).returncode != 0:
            sys.exit("tunnel did not come up; aborting before measuring")
        say("==> measuring (tunnel)")
        report["configs"].append(measure("tunnel", args.runs, args.seconds, args.udp_rate))
        pi_a.script("down")
        pi_b.script("down")

        # --- direct baseline, same two host namespaces ---
        say("==> measuring (direct baseline, no tunnel)")
        baseline_setup()
        if host("ha", "ping", "-c", "2", "-W", "2", HB).returncode != 0:
            say("    baseline path not reachable; skipping baseline")
        else:
            report["configs"].append(measure("baseline", args.runs, args.seconds, args.udp_rate))
    finally:
        for p in pis:
            p.script("down")
        run("bash", os.path.join(NETNS, "n1_fixture.sh"), "teardown", D.RUN)

    (outdir / "benchmark.json").write_text(json.dumps(report, indent=2) + "\n")
    write_summary(outdir / "summary.md", report)
    say(f"==> results in {outdir}")
    return 0


def write_summary(path, report):
    lines = ["# VPTunnel namespace benchmark", "",
             f"- Kernel: `{report['kernel']}` ({report['machine']})",
             f"- {report['runs']} runs x {report['seconds']} s per TCP measurement; "
             f"UDP offered load {report['udp_rate_mbit']} Mbit/s",
             "- Network namespaces on one machine: a controlled comparison, not a hardware "
             "or Wi-Fi measurement (the two-Pi benchmark reports those).", "",
             "| config | TCP fwd Mbit/s | TCP rev Mbit/s | UDP Mbit/s | UDP loss % | ping avg ms |",
             "|---|---|---|---|---|---|"]
    for c in report["configs"]:
        f, rv, u, rtt = (c["tcp_fwd_mbps"] or {}), (c["tcp_rev_mbps"] or {}), (c["udp"] or {}), (c["ping_rtt_ms"] or {})
        lines.append(f"| {c['label']} | {f.get('mean','-')} +/- {f.get('stdev','-')} | "
                     f"{rv.get('mean','-')} +/- {rv.get('stdev','-')} | {u.get('mbps','-')} | "
                     f"{u.get('loss_pct','-')} | {rtt.get('avg','-')} |")
    lines += ["", "`tunnel` crosses the VPN; `baseline` is the same two host namespaces forwarded "
              "directly with no tunnel. The gap is the tunnel's cost: 72 bytes/packet of overhead, "
              "ChaCha20-Poly1305, and the kernel data path (which runs in softirq, so its CPU is not "
              "attributed to the controller process)."]
    path.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
