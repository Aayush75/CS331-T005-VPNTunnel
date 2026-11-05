# control/ — the VPTunnel controller (`vptctl`)

The userspace controller. The kernel module owns every transport record and all key
material after handoff; this crate owns gateway identity, configuration, and the
Noise IK key exchange, and it keeps the gateway's lease in the kernel alive.

`forbid(unsafe_code)`; the one Linux-only module is the Netlink socket, built on
`rustix`.

## Commands

```bash
vptctl genkey --out gw-a.key              # create an owner-only private identity (never overwrites)
vptctl pubkey --key gw-a.key              # print its public key and BLAKE2s fingerprint
vptctl fingerprint <64-hex-public-key>    # fingerprint of a public key
vptctl check-config gw-a.toml [--with-key]  # validate a configuration (and the named key)
vptctl network gw-a.toml                  # links, peer slots, endpoints, prefixes (JSON); installs nothing
vptctl run gw-a.toml --device vpn0        # claim the gateway, load config, run the handshake, hold the lease (Linux, CAP_NET_ADMIN)
```

## What it does

- **Identity.** An X25519 static key per gateway, generated with `genkey` into a
  `0600` file. Peers are pinned by public key in the configuration.
- **Handshake.** `Noise_IK_25519_ChaChaPoly_BLAKE2s` via `snow`, with a VPTunnel
  prologue and framing. The handshake produces two directional ChaCha20-Poly1305
  keys, which are handed to the kernel once; the controller never encrypts data.
- **Configuration.** A TOML file describing the gateway's LAN and outer interfaces,
  its peers (slot, endpoint, pinned key, owned prefixes, path MTU), and the ACL
  rules. `check-config` enforces every rule before the kernel sees it.
- **Lease.** `run` claims the gateway over Generic Netlink and renews a short lease;
  if the controller stops or the socket closes, the kernel closes admission and drops
  the keys within a few seconds. It also re-keys on key age/usage and re-establishes a
  silent peer.

Configuration rules, the Netlink message layer, and the handshake wire format are
described in [`../docs/protocol.md`](../docs/protocol.md).

## Building and testing

```bash
cargo build --release --locked
cargo fmt --check && cargo clippy --all-targets --locked -- -D warnings && cargo test --locked
```

Portable tests run on macOS and Linux. Netlink, nftables, and namespace behaviour are
only exercised on Linux; use the reference VM (see [`../docs/reproducing.md`](../docs/reproducing.md)).
The handshake is checked against an independent Noise vector and a from-scratch
VPTunnel INIT/RESPONSE vector; see `tests/`.
