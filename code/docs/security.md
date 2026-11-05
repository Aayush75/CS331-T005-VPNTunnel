# Security model

VPTunnel makes two separate claims, and keeps them separate:

- **Connectivity** — an authorized host reaches the intended private service through the
  tunnel, and nothing else does.
- **Security** — the tunnel authenticates its peers and protects carried traffic against
  reading, modification, and replay.

Neither proves the other. A packet capture of unreadable bytes is not proof of correct
access control, and a successful handshake does not authorize every address or service.

## What it protects

- **Peer authenticity.** Each gateway has a pinned X25519 static identity. The Noise IK
  handshake authenticates both peers mutually; the initiator's identity is encrypted to
  the responder, and each side is pinned in the other's configuration. There is no
  trust-on-first-use, no group key, and no address-as-identity.
- **Confidentiality and integrity.** Every inner packet is sealed with ChaCha20-Poly1305
  under a per-direction key, with the VPT1 envelope and counter as associated data, so
  the record kind, length, destination locator, and counter are authenticated along with
  the payload.
- **Replay resistance.** Each direction has a monotonic counter and a replay window; a
  replayed or out-of-window record is dropped. Counters never repeat, and a new key only
  comes from a fresh handshake (no symmetric rekey, no counter wrap).
- **Access control.** A default-deny ACL decides which source, destination, protocol,
  and port may cross, and in which initiation direction. Flow witnesses ensure a reply or
  later packet is admitted only because an allowed initiation happened, and a tightened
  policy kills the flow immediately. Tunnel-to-tunnel transit (A→C through B) is denied
  structurally, not merely by a rule.
- **Fail-closed.** Loss of the controller's lease, a revoked or disabled peer, an expired
  key with no re-key, or a corroborated path problem stops new admission for that peer.
  There is no plaintext, alternate-peer, or default-route fallback. With the VPN off, the
  gateway forwards nothing between the LAN and the tunnel.
- **Key hygiene.** Private identity files are owner-only. Keys are excluded from status
  output, logs, and crash diagnostics; transient key buffers are erased, and kernel key
  generations erase their keys once outstanding references drain.

## What it does not protect

- **Traffic analysis.** Outer addresses, UDP port (51888), packet sizes, timing, and
  direction are visible on the wire. Encryption hides the inner packet contents, not the
  shape of the traffic.
- **The underlay itself.** An on-path attacker can drop or delay datagrams (denial of
  service) and can send unauthenticated ICMP path-MTU indications; the gateway requires
  three corroborating indications that each quote a record it really sent before it
  suspends a peer, and a suspended peer recovers with a fresh session — but a persistent
  on-path attacker who can already drop packets can also keep a peer suspended.
- **Link credentials are not VPN credentials.** The Wi-Fi WPA2 passphrase authenticates a
  laptop to a Pi's network; it does not authenticate anything to the remote gateway.
- **No PKI.** Public keys are pinned out of band in the configuration; there is no CA,
  revocation list, or roaming identity.
- **No production hardening.** No daemon privilege separation beyond the kernel/userspace
  split, no congestion control or reliability on the UDP layer, no obfuscation, and no
  claim to withstand a dedicated adversary. It is an educational system qualified within
  this documented envelope.
- **Host suspend / VM snapshot.** A running gateway must not be suspended, snapshotted,
  or cloned: the boot-clock key/lease ageing and the nonce counters make a resume unsafe.
  Recovery is a fresh handshake.

## Scope

IPv4 inner traffic (TCP, UDP, ICMP echo); non-overlapping private prefixes; no inner or
outer NAT; up to four peers per gateway. Not a bridge, not an Internet gateway, no
overlapping-subnet translation, no multicast/broadcast extension.
