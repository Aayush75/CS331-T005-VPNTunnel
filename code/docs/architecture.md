# Architecture

VPTunnel is a gateway-to-gateway, site-to-site VPN. Two gateways connect two private
LANs so that an ordinary host on one LAN reaches an authorized service on the other,
by IP address, over an encrypted tunnel. The gateways stay separate routed networks;
they are not bridged into one Ethernet segment.

```text
 PRIVATE SITE A                 OUTER TRANSPORT                 PRIVATE SITE B
 10.77.1.0/24                   172.31.250.0/24                 10.77.2.0/24

 Host A ── LAN ── Gateway A ===== cable / underlay ===== Gateway B ── LAN ── Host B
 .1.10          .1.1  .250.11                    .250.12  .2.1          .2.10

                Gateway A <===== protected UDP / VPT1 =====> Gateway B
```

- **Inner network:** the two private LANs, `10.77.1.0/24` and `10.77.2.0/24`.
- **Outer transport:** the gateway-to-gateway link (an Ethernet cable between two Pis,
  or an isolated virtual bridge in the namespace testbed), `172.31.250.0/24`.
- **Tunnel:** UDP on port `51888`, carrying the VPT1 record format.

## How a packet crosses

1. Host A sends an ordinary IPv4 packet to `10.77.2.10`. A route on Host A sends
   `10.77.2.0/24` to Gateway A.
2. Gateway A routes that packet onto its virtual interface `vpn0`. The kernel module
   checks it against the default-deny ACL, confirms it belongs to an allowed flow,
   seals it with ChaCha20-Poly1305 under the current send key with a per-direction
   counter, wraps it in a VPT1 record, and sends it as a UDP datagram from port 51888
   to Gateway B's outer address.
3. Gateway B receives the datagram, finds the key by its receive locator,
   authenticates and decrypts it, checks the replay counter, re-checks the ACL, and
   writes the inner packet onto its own LAN toward Host B.
4. The reply retraces the path. "One-directional access" means who may *initiate* a
   flow is controlled; replies to an allowed flow always return.

The tunnel is **routed IPv4**, not a bridge. Anything that works by IP address works
across it — ping, HTTP, SSH, UDP. Broadcast/multicast discovery (mDNS, the "network
neighbourhood") and hostnames without DNS do not cross: the two LANs are different
subnets.

## Where the work happens

```text
  userspace                         kernel (per network namespace)
  ---------                         ------------------------------
  vptctl                            vpt1_foundation.ko
   ├─ identity (X25519)             ├─ vpn0 virtual interface
   ├─ Noise IK handshake    ──────► ├─ UDP socket (port 51888)
   ├─ config + ACL rules    Netlink ├─ record seal / open (ChaCha20-Poly1305)
   └─ lease renewal                 ├─ replay windows, per-direction counters
                                    ├─ ACL + flow witnesses (admission)
                                    ├─ re-key, liveness, fragments, MTU/ICMP
                                    └─ counters / status
```

- **The controller (`vptctl`, userspace)** does everything that is not a data packet:
  it holds the gateway's static identity, validates the configuration, runs the Noise
  IK handshake with each peer, hands the resulting keys to the kernel once, and renews
  a short lease. It never sees or touches a data packet after handoff.
- **The module (kernel)** owns the data path. After the controller hands it the keys,
  the kernel alone seals, authenticates, replay-checks, policy-checks, and forwards
  every record. If the controller dies or its lease lapses, the kernel closes
  admission and drops the keys within a few seconds — the tunnel fails closed.

This split is deliberate: the complex, rarely-run cryptographic handshake stays in
safe userspace Rust, and the fast, always-run per-packet path stays in the kernel
where it avoids a userspace round trip per packet.

## State the kernel owns per peer

- **Generations** — the directional keys. A re-key installs a new generation as a
  *candidate*; both sides confirm it (a four-message activation) before either sends
  application data under it, so a re-key loses no packets. The previous generation
  still receives for a short grace.
- **Replay state** — a per-direction counter and window; a replayed or out-of-window
  record is dropped.
- **ACL snapshot** — an immutable copy of the policy. A configuration change publishes
  a new snapshot atomically; in-flight packets see one snapshot or the next, never a
  half-updated one.
- **Flow witnesses** — a bounded per-peer table recording that an allowed *initiation*
  (a TCP SYN, a UDP datagram, an echo request) happened, so that replies and later
  packets of that flow are admitted while a tightened policy still kills the flow.
- **Lease** — the controller's claim on the gateway, renewed on a short timer.

## Failure behaviour

The design fails closed. Loss of the lease, a revoked or disabled peer, an expired
key with no re-key, or a suspended path all stop new application admission for the
affected peer. There is no plaintext fallback, no alternate-peer fallback, and no
default-route takeover. When the VPN is off, a small nftables ruleset installed by the
bring-up script forwards nothing between the LAN and the tunnel.

## Scope

- IPv4 inner traffic; TCP, UDP, and ICMP echo.
- Non-overlapping private LAN prefixes; no inner or outer NAT.
- Up to four peers per gateway.
- Not a bridge, not an Internet gateway, no overlapping-subnet translation, no
  multicast/broadcast extension.

See [protocol.md](protocol.md) for the wire format and handshake, and
[security.md](security.md) for the security model and its limits.
