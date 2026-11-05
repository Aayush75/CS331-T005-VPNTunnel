# VPTunnel — code

A routed, Layer-3, site-to-site VPN. A client on private LAN A reaches an authorized
service on private LAN B through two gateways and an encrypted tunnel. The service is
unreachable from LAN A without the VPN, and reachable by IP address through it.

```text
 PRIVATE SITE A                 OUTER TRANSPORT                 PRIVATE SITE B
 10.77.1.0/24                   172.31.250.0/24                 10.77.2.0/24

 Host A ── LAN ── Gateway A ===== cable / underlay ===== Gateway B ── LAN ── Host B
 .1.10          .1.1  .250.11                    .250.12  .2.1          .2.10  (HTTP :8080)

                Gateway A <===== protected UDP / VPT1 =====> Gateway B
```

Inner IPv4 packets are selected by routing onto a kernel virtual interface (`vpn0`),
checked against a default-deny policy, sealed with ChaCha20-Poly1305, and carried as
UDP datagrams (port 51888) to the peer gateway, which authenticates, decrypts,
re-checks policy, and delivers them onto its own LAN.

## Components

| Path | What it is | Language |
|---|---|---|
| `kernel/` | The gateway data path: `vpn0`, UDP socket, record seal/open, replay, ACL, flow witnesses, re-key, liveness, fragments, MTU/ICMP. A per-network-namespace Linux kernel module. | C (Linux 6.18) |
| `control/` | `vptctl`: gateway identity, configuration, and the Noise IK key exchange. Loads keys into the kernel and keeps the lease; never handles a data packet. | Rust |
| `protocol/` | The Netlink command/attribute ABI between controller and kernel, as JSON. | — |
| `infra/gateway/` | Scripts to bring a gateway up/down, host the Wi-Fi access point, and the demo Pi configurations. | shell |
| `infra/reference-vm/` | A Lima config for the Linux 6.18 reference VM. | — |
| `tests/` | Kernel unit modules, end-to-end network-namespace tests, and the Rust controller tests. | C / Python / Rust |
| `tools/` | Build-and-test harness, benchmarks, and vector generators. | shell / Python |
| `scripts/` | One-command entry points for building, testing, the demo, and benchmarking. | shell |

## Quick start (reference VM)

Everything that loads the kernel module or creates network namespaces must run as
root on a **disposable** Linux 6.18.y machine. The provided reference VM is that
machine; see [`docs/reproducing.md`](docs/reproducing.md) to obtain and boot it.

```bash
cd code
./scripts/build.sh            # build the module (release + debug) and vptctl
sudo ./scripts/test.sh        # full kernel + namespace + controller test suite
sudo ./scripts/demo-netns.sh  # the before/after demo, in network namespaces
```

## Benchmarks

```bash
sudo ./scripts/bench-netns.sh         # tunnel vs no-tunnel, one machine in namespaces (needs iperf3)
sudo ./scripts/bench-packetsize.sh    # goodput vs datagram size (overhead and MTU effect)
./scripts/bench-pi.sh <laptop-B-ip>   # physical two-Pi path, run from laptop A
./scripts/bench-pi-direct.sh          # raw Pi-to-Pi cable baseline (no tunnel), run on Pi A
```

Results land under `results/<timestamp>-*/summary.md`.

## Two Raspberry Pis

The same system runs on two Raspberry Pi 5s joined by an Ethernet cable, each hosting
a Wi-Fi network for one laptop. Build on each Pi with `./scripts/pi-build.sh` and run
the demo with `./scripts/pi-demo.sh`. Full hardware setup is in
[`docs/pi-setup.md`](docs/pi-setup.md).

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — how a packet crosses, component boundaries, state ownership.
- [`docs/protocol.md`](docs/protocol.md) — the VPT1 wire format, the Noise handshake, and the key schedule.
- [`docs/reproducing.md`](docs/reproducing.md) — obtain the VM, build, and run the tests and demo from scratch.
- [`docs/testing.md`](docs/testing.md) — what each test covers and how to run a single one.
- [`docs/benchmarking.md`](docs/benchmarking.md) — what the benchmarks measure and how to read the numbers.
- [`docs/pi-setup.md`](docs/pi-setup.md) — the two-Pi hardware demonstration, step by step.
- [`docs/security.md`](docs/security.md) — the security model, what it protects, and its limits.

## Requirements

- Linux **6.18.y** with matching kernel headers (the data path uses 6.18 kernel APIs).
- A Rust toolchain (`rustup`) for `vptctl`.
- `iproute2`, `nftables`, `python3`; `iperf3` for benchmarks; `hostapd` and `dnsmasq`
  on the Pis for the Wi-Fi access point.

The build refuses any kernel that is not 6.18.y. Use the reference VM.
