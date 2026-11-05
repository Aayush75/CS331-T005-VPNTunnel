# Protocol: VPT1 wire format and the Noise handshake

VPTunnel carries one inner IPv4 packet per authenticated record, as a UDP datagram
on port **51888**. Key exchange is Noise IK; the data records use ChaCha20-Poly1305.
All project integers are unsigned big-endian (the AEAD nonce is the one exception,
below). Fields are serialized explicitly, never copied from C structs.

## Common envelope (16 bytes)

Every datagram — INIT, RESPONSE, and RECORD — starts with the same 16-byte envelope:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | Magic | `"VPT1"` |
| 4 | 1 | Kind | `1` = INIT, `2` = RESPONSE, `3` = RECORD |
| 5 | 3 | Flags / reserved | zero |
| 8 | 8 | Destination receive locator | zero only in INIT; otherwise the peer's nonzero local locator |

A **receive locator** is a uniformly random nonzero 64-bit value the kernel assigns,
collision-checked against every retained locator in that namespace. It is a
demultiplexing hint — which key generation a record is for — not a credential, and it
is bound to one peer, one generation, and one incarnation. There is no sequential ID
and no endpoint learning.

Unknown magic, kind, impossible length, trailing bytes, or oversize input is dropped.
UDP checksums are required both ways. Outer IPv4 options and outer fragmentation are
dropped at the underlay boundary.

## Handshake: `Noise_IK_25519_ChaChaPoly_BLAKE2s`

The controller runs the Noise IK exchange in userspace (via `snow`), with an 80-byte
VPTunnel prologue binding the domain, a reserved word, and both parties' static public
keys, so roles fix the public-key order. The initiator's static key is encrypted to the
responder under IK, so the responder is pinned (there is no trust-on-first-use), and the
responder's identity is pinned in the initiator's configuration.

- **INIT** — 224 bytes of UDP payload (16-byte envelope + 208 of Noise message +
  payload). Destination locator is zero.
- **RESPONSE** — 176 bytes. It echoes the mode, attempt, initiator locator, parent and
  path budget, and supplies the responder's locator and incarnation. A mismatched path
  budget is an establishment failure, never a silent downgrade.

An all-zero X25519 DH result is rejected in constant time before the key schedule.
Application data is never carried in a handshake message.

### Key schedule

Noise Split returns `(k1, k2)`. The **initiator** installs TX = `k1`, RX = `k2`; the
**responder** installs TX = `k2`, RX = `k1`. These are the only keys; no extra KDF is
introduced, and Noise's symmetric-only rekey is never used — a new key only comes from a
fresh IK exchange. The controller hands the two 32-byte keys to the kernel once, over a
privileged, namespace-scoped Generic Netlink transaction that also carries the peer
incarnation, configuration revision, attempt ID, handshake hash, locators, role, and
continuity decision. Only the kernel ever allocates a transport counter or encrypts a
record, so userspace and kernel can never share a nonce.

## Protected records (kind = RECORD)

The datagram body is:

```text
counter[8] || ciphertext || tag[16]
```

- The **first 24 bytes** (the 16-byte envelope + the 8-byte counter) are the AEAD
  associated data. This authenticates the kind, flags, body length, destination
  locator, and counter. Outer IP/UDP addresses are statically filtered but are not in
  the AAD.
- **AEAD:** RFC 8439 ChaCha20-Poly1305, 32-byte key, 12-byte nonce, full 16-byte tag
  (the kernel provider `rfc7539(chacha20,poly1305)`).
- **Nonce:** `four zero bytes || little_endian_u64(counter)` — the one little-endian
  field, as RFC 8439 specifies.
- **Counters** start at zero per directional key, are reserved atomically before
  encryption, and are never reused even if sending fails. Data and control records
  share a direction's counter space.

The decrypted plaintext begins with `subtype[1] || flags[1]=0 || payload_len[2]`,
then exactly `payload_len` bytes. Subtype `1` (IPv4) carries one complete inner packet
or fragment whose IP total length equals `payload_len`. Other subtypes are fixed-size
control records (re-key activation, liveness PING/PONG, path indications). Mismatched
lengths and unknown subtypes are rejected **after** authentication.

## Overhead and MTU

Per packet the tunnel adds **72 bytes**: outer IPv4 (20) + UDP (8) + VPT1 envelope (16)
+ counter (8) + plaintext prefix (4) + tag (16). With a 1500-byte outer MTU, `vpn0`
advertises an inner MTU of **1428**. Inner datagrams that would exceed the outer MTU
once encapsulated are dropped by design — the tunnel does not fragment the outer packet.

## Re-key, liveness, and failure

- **Re-key** happens on key age or usage, or when the kernel asks. A new generation is
  installed as a *candidate* that receives but does not yet carry application data; a
  four-message activation (CONFIRM / READY / COMMIT / COMMITTED) confirms both sides
  hold it, then each switches sending — so a re-key loses no packets. The old generation
  still receives for a short grace.
- **Liveness** is a kernel PING/PONG; a peer that stops answering is closed, and the
  controller re-establishes it. The controller also re-keys a silent peer so a responder
  restart recovers in seconds rather than at the next scheduled re-key.
- **Lease:** the controller renews a short lease; if it stops, the kernel closes
  admission and drops keys within a few seconds. Nothing falls back to plaintext.

The exact Netlink command and attribute IDs are in
[`../protocol/netlink-v1.json`](../protocol/netlink-v1.json).
