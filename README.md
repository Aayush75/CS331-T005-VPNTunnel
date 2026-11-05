# VPTunnel

## Team ID — T005

## Project ID — 1

### Team Members

1. Aayush Kuloor — 24110004
2. Anvit Aggarwal — 24110048
3. Dhru Harsh Darshakbhai — 24110107
4. Raj Amit Modi — 24110282

---

VPTunnel is a routed, Layer-3, site-to-site VPN. An ordinary client on one private
LAN reaches an authorized private service on another private LAN through two
gateways, over an encrypted tunnel. Without the VPN the service is unreachable from
the other network; with it, the client reaches the service by IP address as if the
two networks were connected.

The gateway data path — authentication, replay protection, policy, and
ChaCha20-Poly1305 encryption of every inner packet — runs in a Linux kernel module.
A small userspace controller (`vptctl`) holds each gateway's identity and runs the
Noise IK handshake; it never touches a data packet after key handoff.

All source, tests, scripts, and documentation live under [`code/`](code/). Start
with [`code/README.md`](code/README.md) for the overview and
[`code/docs/reproducing.md`](code/docs/reproducing.md) to run everything.

This is an educational system qualified within a documented operating envelope, not
a production VPN.

Native-hardware benchmark tables, CPU measurements, figures, and reproduction files
are included in the [native-system results archive](code/results/report-results/native-benchmarks-2026-11-06.zip)
and [physical Pi results archive](code/results/report-results/pi-physical-results-2026-11-06.zip).
