# Reference VM

The Linux 6.18 reference VM used to build, test, and demonstrate VPTunnel. It is the
disposable machine every root-level step expects. See
[../../docs/reproducing.md](../../docs/reproducing.md) for how to boot it and run
everything; the helper scripts here (`run-vm.sh`, `run-vm-macos.sh`, `reset-vm.sh`) and
the Lima template (`lima-vpt1.yaml`) start it.

## Facts

| | |
|---|---|
| Kernel | `6.18.0-vpt1-ref` (Linux 6.18.y, x86-64) |
| Base | Debian 13 (Trixie) |
| Page size | 4096 bytes |
| Suggested sizing | 2 vCPUs, 4 GiB RAM, 20 GiB disk |
| Default user | `testuser` (password `password`, has `sudo`) |
| SSH | host port 2222 forwarded to the guest |

## Image

- **Filename:** `vpt1-ref-6.18-x86_64.qcow2`
- **SHA-256:** `f015bc123731185c5f6db9c3673a86a1c45cffd92ca39811a3dffad27497cc88`
- **Download:** <https://drive.google.com/file/d/16ghaL8cqNT5avPYj0CVNKsPrq5PGa0l3/view?usp=sharing>

Verify the download:

```bash
sha256sum vpt1-ref-6.18-x86_64.qcow2
```

## Installed facilities

- Matching kernel headers, build symlink, and `Module.symvers`.
- Crypto AEAD `rfc7539(chacha20,poly1305)` (12-byte IV, 16-byte tag).
- `nftables` (with `nft_ct`), `iproute2` (`ip netns`), `conntrack-tools`.
- `tcpdump`, `ethtool`, `iw`, `kmod`; `gcc`, `make`, `python3`; `iperf3`.
- A Rust toolchain for `vptctl`.

## Running

- **Linux (KVM):** `./run-vm.sh` — boots with a copy-on-write overlay so the downloaded
  image stays unmodified.
- **macOS (QEMU):** `./run-vm-macos.sh` — same overlay approach; x86-64 is emulated under
  QEMU on Apple Silicon, so it is a functional environment, not a performance baseline.
- **macOS (Lima):** `lima-vpt1.yaml` — download the image first and point the template's
  `location` at it.
- **Reset:** `./reset-vm.sh` discards the overlay and returns to the pristine image.
