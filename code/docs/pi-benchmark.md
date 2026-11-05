# Prepared Pi-to-Pi Ethernet benchmark

This is separate from the Wi-Fi/laptop demonstration. Each Pi generates/receives
traffic locally. A dummy interface holds a gateway address plus a LAN-host alias;
the configured gateway address itself is reserved by normal-data policy.

| | Pi A (`vpn-a`) | Pi B (`vpn-b`) |
|---|---|---|
| Ethernet underlay | 172.31.250.11/24 | 172.31.250.12/24 |
| Dummy gateway address | 10.77.1.1/24 | 10.77.2.1/24 |
| **VPN benchmark endpoint** | **10.77.1.10** | **10.77.2.10** |
| Role | initiator | responder |

The prepared repository is `/home/cn/CS331-T005-VPNTunnel` on each Pi. The native
module, controller and helper copies are installed under `/usr/local`. Machine
configurations and owner-only private keys live under `/etc/vpntunnel`; no private
key is copied into the repository or evidence. `benchmark.toml` uses `vptbench0`;
`demo.toml` uses `wlan0`. Example demo configs in each checkout have the new public
peer identities filled in. Old project directories/processes were removed.

Both Pis run native aarch64 Linux 6.18.34+rpt-rpi-2712 with 16 KiB pages and matching
headers/GCC 14. Rust 1.93.1 is selected on both. Dependencies are vendored outside the
checkout at `/home/cn/.local/share/vpt-build/vendor`; the checkout's `.cargo/config.toml`
selects offline builds. From the repo's `code/`, `./scripts/pi-build.sh` rebuilds
natively without Internet. Reinstall built binaries before using the installed helpers
if you change the source later.

## Final wiring and management

Connect the two Pi Ethernet ports **directly**. Their saved NetworkManager profile
`vpt-underlay` brings up the static Ethernet address automatically. Verify negotiated
speed on both with `sudo ethtool eth0`. Leave their Wi-Fi in station mode for SSH
management; no AP or laptop data path is involved in this experiment.

The temporary provisioning topology used this laptop as a bridge, with an RTL8152
USB Fast Ethernet adapter on the Pi A side (100 Mbit/s) and gigabit on the Pi B side.
Ping loss occurred on that plain underlay too. Setup connectivity checks from that
arrangement are not final benchmark results. Check speed/loss again after the direct
cable is installed; do not treat these provisioning measurements as Pi VPN capacity.

## Start/stop

The enabled `vpt-benchmark-net.service` restores `vptbench0` and both local addresses
at boot. The VPN itself stays **off at boot**, with an unreachable far-LAN route so
benchmark packets cannot fall back through the campus Wi-Fi default route.

```
# Pi B first
sudo vpt-bench up
# Pi A second
sudo vpt-bench up
# either Pi
sudo vpt-bench status
sudo vpt-bench ready
```

`sudo vpt-bench down` stops the local VPN, removes vpn0 and restores the unreachable
far-LAN route. `sudo vpt-bench net-down` also removes the dummy addresses, freeing
these subnets before switching to the Wi-Fi demo. `net-up` restores the benchmark
addresses. Never run the AP demo and dummy-LAN benchmark simultaneously.

Bind every measurement explicitly:

```
# Direct: server on B; client on A
iperf3 -s -B 172.31.250.12
iperf3 -c 172.31.250.12 -B 172.31.250.11 -J

# VPN: server on B; client on A
iperf3 -s -B 10.77.2.10
iperf3 -c 10.77.2.10 -B 10.77.1.10 -J

# A: verify routing
ip route get 10.77.2.10 from 10.77.1.10
ping -I 10.77.1.10 -c 5 10.77.2.10
```

The VPN route must use vpn0, while outer peer traffic uses eth0. The `.1` gateway
addresses are not benchmark endpoints. TCP MSS follows vpn0's 1428-byte inner MTU;
keep outer path MTU at 1500 and UDP payloads at or below 1400 bytes unless explicitly
testing the MTU boundary.

## CPU for every physical experiment

Start a distinct recorder on **both** Pis before each phase, including an idle
observation. This standalone command works as user cn:

```
vpt-cpu-monitor --output "$HOME/results/pi-a-PHASE-cpu" --seconds 3600
```

Use `pi-b` in B's output directory name. Ctrl-C or SIGTERM saves the raw 0.5-second
system/per-core CPU snapshots and summary. Record experiment start/end UTC times,
clock synchronization, temperatures, throttling, governor and Ethernet/offload state.
Whole-host CPU includes endpoint generation and VPN processing; do not present it
as VPN-exclusive CPU. Copy result files after traffic measurements finish.

Setup evidence is in `/home/cn/pi-setup/` on each device and included in this repo's
`code/results/report-results/pi-setup-2026-11-06.zip`. It includes failed initial checks and the correction to
LAN-host aliases, so no setup failure is silently omitted. Formal benchmark runs
must use fresh result directories and the final direct Ethernet topology.
