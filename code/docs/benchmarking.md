# Benchmarking

Four benchmarks, each writing a Markdown `summary.md` (the namespace ones also JSON) under
`results/<timestamp>-*/`. The two namespace benchmarks run on **one machine** inside
network namespaces (no Pis, no Wi-Fi, no cable) and are fully reproducible on a Linux
6.18.y machine; the two Pi benchmarks need the two-Pi setup.

All need `iperf3`. It is **not** preinstalled on the reference VM: install it first with
`sudo apt install iperf3` (the VM needs internet access for this). Run the namespace
ones as root.

## 1. Throughput and latency — `scripts/bench-netns.sh`

```bash
sudo ./scripts/bench-netns.sh [--runs 5] [--seconds 15] [--udp-rate 100]
```

It stands up the five-namespace topology, brings the VPN up with the real controller
and kernel module, and measures the client→service path in two configurations:

- **tunnel** — traffic crosses the VPN.
- **baseline** — the *same two host namespaces* reachable directly through the underlay
  with ordinary IPv4 forwarding and no tunnel.

Measuring both on the same topology means the gap is the tunnel's cost, not a difference
between two unrelated paths. Per configuration it records TCP goodput both directions
(`--runs` iperf3 runs, mean ± stdev), a UDP throughput/loss point at `--udp-rate`, and
ping RTT. The summary table has one row per configuration.

**How to read it.** The tunnel row is lower than the baseline by the cost of: 72 bytes
of per-packet overhead, ChaCha20-Poly1305 on every packet, and the extra kernel data-path
work. The kernel data path runs in softirq, so its CPU is not attributed to the `vptctl`
process — do not read controller CPU as the tunnel's total CPU. Numbers from namespaces
on one machine are a controlled comparison, not a hardware or Wi-Fi figure.

## 2. Packet-size sweep — `scripts/bench-packetsize.sh`

```bash
sudo ./scripts/bench-packetsize.sh [--seconds 8] [--rate 200]
```

With the VPN up, it sends UDP at a fixed offered rate across a range of datagram sizes
(64 B up to the 1428-byte inner MTU) and reports delivered goodput, loss, and packet
rate for each. It shows two things the design predicts:

- **Small datagrams pay the fixed 72-byte overhead on every packet**, so their delivered
  goodput is lower and the packet rate is the limiting factor.
- **Inner datagrams that would exceed the outer MTU once encapsulated are dropped** — the
  tunnel does not fragment the outer packet. This is why the project keeps the inner MTU
  at 1428 and large UDP datagrams over the limit do not cross.

## 3. Two-Pi physical path — `scripts/bench-pi.sh`

Run from laptop A while the two-Pi demo is up (see [pi-setup.md](pi-setup.md)), with an
`iperf3` server on laptop B:

```bash
# laptop B
iperf3 -s -1 -B <laptop-B-ip>
# laptop A
./scripts/bench-pi.sh <laptop-B-ip> [laptop-A-ip] [--runs 5] [--seconds 20]
```

It measures ping RTT and TCP goodput both directions over the real path (laptop Wi-Fi +
the Pi-to-Pi cable), prompting you to restart the one-shot iperf3 server between runs.
Record the negotiated Wi-Fi rate (`iw dev wlan0 link` on each Pi) and the cable link
speed (`ethtool eth0`) alongside the numbers: Wi-Fi throughput varies with signal and
contention, so a single run is not a stable figure. Run it once with the VPN up (numbers)
and once with the VPN down at the Pis (the transfers fail — that is the isolation half of
the demo, not a benchmark value).

## 4. Two-Pi direct baseline — `scripts/bench-pi-direct.sh`

The reference for benchmark 3. It measures Pi A to Pi B over the Ethernet cable alone,
with no tunnel, no Wi-Fi and no laptops, so it shows what the physical link can carry.
Run it on Pi A; Pi B runs a persistent server (no restarts between runs):

```bash
# Pi B
iperf3 -s -B 172.31.250.12
# Pi A
./scripts/bench-pi-direct.sh [PI_B_IP] [PI_A_IP] [--runs 5] [--seconds 20]
```

Defaults are the cable addresses from [pi-setup.md](pi-setup.md) (`172.31.250.11` and
`.12`). These are underlay addresses, so the traffic never enters the tunnel whether the
VPN is up or down. It reports ping RTT and TCP goodput in both directions.

**How to read it.** Benchmark 3 crosses Wi-Fi twice, the cable, and the tunnel; this one
crosses only the cable. Their difference is therefore tunnel cost *plus* Wi-Fi, not
tunnel cost alone. For the tunnel's cost in isolation, use benchmark 1 (same topology
with and without the tunnel). Record `ethtool eth0` for the cable speed.

## Method notes to report

- State the kernel, CPU, and whether KVM/emulation was used (namespace runs on the VM
  under emulation are comparisons, not absolute hardware throughput).
- `iperf3 -O 2` omits the first 2 seconds (TCP ramp-up); several runs give a spread.
- The baseline and tunnel use the identical topology and hosts, so their difference is
  attributable to the tunnel rather than to routing.
