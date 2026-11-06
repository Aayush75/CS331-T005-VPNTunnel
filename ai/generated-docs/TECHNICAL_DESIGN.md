# VPNTunnel technical design

**Team:** T005<br>
**Date:** 31 October 2026<br>
**Version:** 1.1 — Linux 6.18 and Pi 5 deployment target<br>
**Profile:** VPT1, IPv4 site-to-site, static directly reachable UDP endpoints<br>
**Status:** Selected technical specification; implementation, interoperability, security review, and hardware qualification remain to be demonstrated.

## 1. Scope, authority, and decisions

This document resolves the protocol, kernel integration, state ownership, and operating-profile choices left open by [DESIGN_PLAN.md](DESIGN_PLAN.md). It specifies interfaces, packet representations, algorithms, invariants, and observable outcomes. It is not a coding schedule or a deployment procedure.

[TEAM_WORK_PLAN.md](TEAM_WORK_PLAN.md), [CODING_STANDARDS.md](CODING_STANDARDS.md), and [TEST_PLAN.md](TEST_PLAN.md) separately define the four-person implementation responsibilities, low-level review rules, namespace harness and qualification gates. They preserve this technical profile; their proposed work and tests are not completed implementation evidence.

The README's private-service objective remains primary. **Topology decision, 5 November 2026:** the physical test follows the README's own example. Each Pi hosts a Wi-Fi network for its own laptop (the laptop LAN) and the two Pis are joined by an Ethernet cable, which is the **outer gateway transport**. This replaces the 28 October proposal (laptops on Ethernet, Pi A's Wi-Fi access point joined by Pi B as a station), which is withdrawn. The site-to-site routing objective is unchanged. Initial testing remains Linux network namespaces.

The selected design is a project-owned Linux virtual-interface/UDP tunnel, not an existing VPN renamed. It uses an existing Noise handshake implementation and Linux cryptographic/forwarding facilities. VPT1's framing and lifecycle are project protocol choices and require review; selecting them is not evidence that a new complete VPN protocol has inherited a standardized VPN's assurance.

| Area | Selected decision |
|---|---|
| Key exchange | `Noise_IK_25519_ChaChaPoly_BLAKE2s`, Noise revision 34; no PSK, fallback, or early application data |
| Exchange component | Userspace `snow` 0.10.0, default primitive resolver plus an all-zero-DH-result validation wrapper; `risky-raw-split` confined to one-time key handoff |
| Data protection | RFC 8439 ChaCha20-Poly1305, 32-byte directional keys, 12-byte nonce, full 16-byte tag |
| Wire protocol | VPT1 framing, UDP port 51888; one inner packet/fragment per authenticated record |
| Kernel integration | GPL-compatible external Linux module; per-network-namespace Layer-3 `vpn0`, kernel UDP socket, Crypto API, Netfilter hooks |
| Platform API baseline | Linux 6.18.y, x86-64 reference VM and ARM64 including Pi 5 16 KiB pages; no kernel-source patch or private symbol export |
| Policy | Module-owned immutable ACL snapshots and flow-admission witnesses; Linux conntrack validates transport state; a small nftables ruleset (`infra/gateway/vpt-gateway.sh`) forwards only between the LAN and `vpn0` |
| Fragment processing | Module-owned, bounded, peer-scoped normalization before conntrack for protected inner fragments |
| Initial outer path | Isolated namespace underlay; final outer path is the Ethernet cable between the Pis |
| Default IP MTUs | Qualified outer PMTU 1500; inner MTU 1428; exact data expansion 72 bytes |
| Scale limit | Four remote peers per gateway; eight gateway instances per namespace test host |
| Failure policy | No new application admission after an eligibility deadline/revocation; no plaintext, NAT, alternate-peer, or default-route fallback |

Exact distribution builds, kernel patch identifiers, package hashes, Wi-Fi hardware/driver capabilities, and measured results are deployment facts to record, not alternative technical architectures still awaiting selection. Unsupported facilities fail qualification; they do not cause an automatic protocol change.

## 2. The complete test topology this profile supports

### 2.1 Physical two-site test

```text
 PRIVATE SITE A                    OUTER CABLE                    PRIVATE SITE B
 10.77.1.0/24                    172.31.250.0/24                   10.77.2.0/24

 Laptop A ~~ Wi-Fi ~~ Pi A  ======= Ethernet cable =======  Pi B ~~ Wi-Fi ~~ Laptop B
 .1.10     (network A)  LAN .1.1  eth0 .250.11   eth0 .250.12  LAN .2.1  (network B)  .2.10
                                                                                  HTTP :8080

                 Pi A <========= protected UDP/VPT1 =========> Pi B
```

Each Pi's Wi-Fi interface (the laptop LAN, `10.77.x.1/24`) and its Ethernet interface (the outer cable, `172.31.250.1x/24`) are routed interfaces in different subnets. Neither Pi bridges them. Each laptop joins only its own Pi's Wi-Fi network; no laptop uses the cable, the other Pi's network, tethering or a proxy during the service-isolation experiment. The cable is not an Internet uplink and requires no NAT traversal.

Each Pi hosts its Wi-Fi network with `infra/gateway/vpt-ap.sh` (hostapd and dnsmasq): a static IPv4 address on the LAN interface, WPA2-Personal with AES/CCMP, a hardware-supported legal channel, DHCP for the laptop only (`.50` to `.200` of the LAN, with the Pi as default gateway and no DNS) and no Internet-sharing/NAT service. A convenient hotspot configuration that silently enables NAT is not this topology. These link-security settings do not replace VPT1 peer authentication or encryption. A Pi's own campus Wi-Fi connection ends while its radio hosts the network (the radio has one role), so the Pi is managed over the cable or from a laptop on its own Wi-Fi network. See [docs/DEMO.md](docs/DEMO.md) and [docs/PI_SETUP.md](docs/PI_SETUP.md).

The Pi 5 radio advertises AP mode, and AP mode was run on one Pi on 4 November 2026 (a phone joined, received a DHCP address and answered pings). Each Pi is qualified separately; neither is assumed to qualify the other.

A physical third gateway C would need a switch on the cable segment, and its outer exchange with B must not be blocked; the two-Pi core test needs only the direct cable. The cable is an intentional shared underlay failure point: unplugging it removes the outer path for both gateways and is not a selective peer failure. A Pi's Wi-Fi network going down affects only that Pi's laptop.

| Role | LAN address | Outer address / role | Remote route |
|---|---|---|---|
| Host A | `10.77.1.10/24` | None | `10.77.2.0/24` via `10.77.1.1` |
| Gateway A | `10.77.1.1/24` (Wi-Fi) | `172.31.250.11/24` (Ethernet cable) | B prefix through peer B on `vpn0` |
| Gateway B | `10.77.2.1/24` (Wi-Fi) | `172.31.250.12/24` (Ethernet cable) | A prefix through peer A on `vpn0` |
| Host B | `10.77.2.10/24` | None | `10.77.1.0/24` via `10.77.2.1` |
| Host A2, extension | `10.77.1.11/24` | None | Same remote route as A |
| Host B2, extension | `10.77.2.11/24` | None | Same remote route as B |
| Gateway C, M scenario | `10.77.3.1/24` | `172.31.250.13/24` | B prefix through peer B |
| Host C | `10.77.3.10/24` | None | B prefix via `10.77.3.1` |

There is no client or gateway default route in the isolated test profile. The gateways' connected Ethernet routes reach their outer peers independently of `vpn0`. Inner SNAT/DNAT and outer masquerading are absent. Local console or a separately permitted local-LAN management connection is used for administration, never an HTTP proxy or forwarding shortcut for the main client request.

### 2.2 Namespace equivalent

The minimal fixture uses `host-a`, `gw-a`, `underlay`, `gw-b`, and `host-b`. Two host↔gateway veth pairs represent the laptop LANs (the Wi-Fi networks). An unnumbered bridge in `underlay`, attached only to gateway outer veth ports, represents the Ethernet cable between the Pis. The initial/root namespace has no data-plane attachment or NAT/proxy path into this fixture.

The multiple-host fixture adds `host-a2`, `host-b2`, and switch-only `lan-a`/`lan-b` namespaces. Each site's bridge joins only its two hosts and its own gateway LAN port. The independent-peer fixture additionally uses `gw-c`/`host-c` with an A–B and a C–B relationship. Every gateway uses a separate identity and namespace-local state even when all bind local UDP port 51888.

These fixtures reproduce routing and policy boundaries, not radio behavior or independent-machine failures. A kernel module unload affects all namespaces on that host and is not a one-peer restart test.

### 2.3 Multiple hosts and independent peers in the final evidence

The two-Pi/two-laptop physical layout establishes the core service path. To physically attach A2 and B2 as additional computers, each Ethernet LAN needs its own switch and additional host. Separately labeled host namespaces on the laptops are an alternative source of functional multi-host evidence, not proof of four physical laptops. No additional site gateway is needed for that extension.

Two Pi endpoints do not prove independent multi-peer support. That claim uses the three-gateway namespace fixture, or an additional independently keyed physical/virtual gateway with its own legitimate underlay attachment. Adding C does not permit A↔C transit through B.

The service policy is:

| Initiating client | B `10.77.2.10:8080/TCP` | B2 `10.77.2.11:8080/TCP` |
|---|---|---|
| A `10.77.1.10` | Allow | Deny |
| A2 `10.77.1.11` | Allow | Allow |
| C `10.77.3.10`, when configured | Allow | Deny |

Replies require the corresponding admitted flow. New B/B2→A/A2 connections, unlisted services, gateway-local services, and peer transit are denied. Each B listener must also work from a local check originating at Gateway B's LAN address. Response bodies identify B versus B2 and echo a fresh request identifier.

## 3. Identity and cryptographic selection

Each gateway has one independently generated X25519 static key pair. Its raw 32-byte public key is the pinned identity; a human-readable fingerprint is BLAKE2s-256 of that public key, shown as 64 lowercase hexadecimal digits. A and C may both pin B's identity. A–B and C–B nevertheless have independently derived session keys and replay state.

Static and ephemeral secret generation uses the operating system's initialized cryptographic random source, never a time/PID seed or deterministic production fixture. The responder compares the authenticated initiator static key with the identity configured for the source endpoint. A known source address alone cannot authenticate a peer.

The exchange is the unmodified Noise IK pattern, instantiated as `Noise_IK_25519_ChaChaPoly_BLAKE2s`. The initiator already knows the responder static key; this profile also provisions the expected initiator key at the responder. The fixed prologue is:

```text
ASCII("VPNTUNNEL-V1") || four zero bytes || initiator_static[32] || responder_static[32]
```

The domain string plus zero bytes is 16 bytes; the entire prologue is 80 bytes. Roles fix the public-key order. No endpoint address, route advertisement, password, or mutable ACL is a cryptographic identity. Every all-zero X25519 result is rejected by the validation boundary described below. Application payload is never carried in a handshake.

Noise supplies this exchange's transcript/key derivation; it does not supply VPT1's record parser, replay window, ACLs, or lifecycle. VPT1 uses the two 32-byte Noise Split outputs for its explicitly specified AEAD record layer. That record layer uses non-empty associated data and is **not** a claim of byte compatibility with Noise's default empty-AD transport API, WireGuard, or the previous project. [Noise specification](https://noiseprotocol.org/noise.html)

### 3.1 Exchange implementation and key handoff

The selected userspace component is `snow` 0.10.0. Its `risky-raw-split` feature exposes `dangerously_get_raw_split`; the versioned upstream source defines this facility. It is used only after the complete IK exchange and pinned-key checks, once per attempt. The implementation must not subsequently use Snow's transport ciphers with those same keys. [Versioned handshake source](https://raw.githubusercontent.com/mcginty/snow/v0.10.0/src/handshakestate.rs)

Default features provide the selected primitives and OS random source; `risky-raw-split` is additionally enabled. A validating `CryptoResolver` delegates primitives/RNG to Snow's default resolver and wraps its DH operation: require the expected public-key length, then reject an all-zero 32-byte DH result with a constant-time check before it reaches Noise's key schedule. This is explicit because the versioned default DH provider returns its computed bytes without that extra rejection. The wrapper uses the library's public resolver interface and does not replace curve arithmetic or modify the IK pattern. [Provider source](https://raw.githubusercontent.com/mcginty/snow/v0.10.0/src/resolvers/default.rs), [resolver interface](https://docs.rs/snow/0.10.0/snow/struct.Builder.html)

Let Split return `(k1, k2)`. The initiator installs TX=`k1`, RX=`k2`; the responder installs TX=`k2`, RX=`k1`. No additional project KDF or modified Noise handshake is introduced. Only the kernel allocates transport counters or encrypts transport records, including control records. This prevents userspace and kernel from accidentally sharing a nonce counter.

The trusted controller hands the keys to the kernel through a privileged, namespace-scoped Generic Netlink transaction containing the peer incarnation, configuration revision, attempt ID, full handshake hash, local/remote receive locators, role, continuity decision, and directional key material. Key installation is conditional on that exact pending attempt still being authorized. A repeated identical installation acknowledges existing state without resetting counters; conflicting reuse is rejected.

Private identity files are owner-only regular files. Secrets are excluded from normal status/dumps, logs, multicast Netlink messages, and crash diagnostics. Project-owned transient key buffers are erased and completed handshake objects are destroyed after handoff; kernel generations erase keys after outstanding references drain. Package/transitive-dependency hashes are recorded in the qualification manifest. The selected package version is a design baseline, not a claim that dependency review has been completed.

Explicit erasure applies to all project-owned buffers. Upstream library destructors, compiler-created copies, and provider internals are not assumed to erase secrets merely because a Rust/C object is dropped. The dependency/key-ownership audit must establish the actual ephemeral/chaining-key cleanup and disable core dumps for the controller; any remaining erasure limitation is reported with the forward-secrecy claim. No document can certify that audit from a library name alone.

## 4. Wire format and parsing

All project integers are unsigned big-endian, except the AEAD nonce encoding explicitly defined below. Fields are serialized, not copied from native C structures. Outer transport is IPv4 UDP, source and destination port **51888**. Each socket is bound to its gateway's configured outer address and underlay interface in its own network namespace. Unexpected source IP/port, destination IP/port, or interface is rejected before protocol work.

### 4.1 Common envelope

| Offset | Bytes | Field | Rule |
|---|---|---|---|
| 0 | 4 | Magic/version | ASCII `VPT1`, hex `56 50 54 31` |
| 4 | 1 | Message kind | 1=INIT, 2=RESPONSE, 3=RECORD |
| 5 | 1 | Flags | Exactly zero |
| 6 | 2 | Body length | Exactly UDP payload length minus 16 |
| 8 | 8 | Destination receive locator | Zero only for INIT; otherwise a nonzero local pending/current/retiring locator |

The envelope is 16 bytes. There is no native-alignment padding, optional field, compression, version negotiation, or concatenated record. Unknown magic, kind, flags, impossible lengths, trailing bytes, or oversize input is dropped. UDP checksums are required in both directions. Transport IPv4 options and outer fragmentation are outside the admitted receive profile and are dropped at the early underlay boundary.

Receive locators are uniformly random nonzero 64-bit values allocated by the kernel, collision-checked against all retained locators in that namespace. They are demultiplexing hints, not credentials. A locator is bound to one peer, one key generation, and one local incarnation. There is no sequential-ID identity claim or automatic endpoint learning.

### 4.2 INIT and RESPONSE

Their body is `attempt_id[16] || noise_message`. The kernel chooses a fresh random 128-bit attempt ID for each new attempt. Initiator A starts A–B; initiator C starts C–B. The expected peer is selected by the configured endpoint, not by accepting a remote route/identity claim.

Both Noise messages have an exactly 96-byte application payload:

| Payload offset | Bytes | Meaning |
|---|---|---|
| 0 | 1 | Mode: 0=fresh, 1=authorized rekey |
| 1 | 7 | Reserved, all zero |
| 8 | 16 | Attempt ID, equal to the clear body copy |
| 24 | 16 | Sender's controller/boot incarnation ID |
| 40 | 8 | Initiator's receive locator |
| 48 | 8 | Responder's receive locator; zero in INIT |
| 56 | 32 | Parent session ID; zero for fresh, old full handshake hash for rekey |
| 88 | 4 | Operating profile ID, exactly 1 |
| 92 | 2 | Configured bidirectional outer path budget `P` |
| 94 | 2 | Pair receive inner cap, exactly `P − 72` |

The RESPONSE echoes the mode, attempt, initiator locator, parent, and profile and supplies the responder locator/incarnation. Its path budget must match the configured value at both endpoints; a mismatch is an establishment failure, not a downgrade. A changed remote incarnation forces fresh establishment; it cannot preserve old flow permission.

For this fixed IK suite, INIT's Noise message is 192 bytes: 96 bytes of IK key/tag expansion plus the 96-byte payload. RESPONSE's Noise message is 144 bytes: 48 plus 96. Thus INIT is 224 bytes of UDP payload / 252 bytes of outer IPv4, and RESPONSE is 176 / 204 bytes. These sizes are mandatory parser expectations, not suggested targets. The RESPONSE envelope locator equals the authenticated initiator locator. INIT's envelope locator is zero.

### 4.3 Protected records

For RECORD, the body is `counter[8] || ciphertext || tag[16]`. The first 24 UDP bytes—common envelope plus transmitted counter—are AEAD associated data. This explicitly authenticates kind, flags, exact body length, destination locator, and counter. Outer IP/UDP addresses are statically filtered but are not placed in AEAD associated data.

```text
nonce = four zero bytes || little_endian_u64(counter)
ciphertext, tag = ChaCha20-Poly1305(key, nonce, AAD=first_24_bytes, plaintext)
```

Counters start at zero independently for each directional key, are reserved atomically before encryption, and are never reused even if encryption/output fails. Data and control share that direction's counter space. Only completed fresh IK exchanges create new keys. The profile never calls Noise's symmetric-only Rekey function, wraps counters, or restores live key state from disk.

The authenticated plaintext starts with `subtype[1] || flags[1]=0 || payload_len[2]`, followed by exactly that many payload bytes. There is no record padding. IPv4 subtype 1 carries exactly one complete inner packet or fragment; its inner total length must equal `payload_len`. Other subtypes are the fixed-size control messages below. Reject mismatched lengths and unknown subtypes after authentication.

The AEAD primitive is RFC 8439 ChaCha20-Poly1305 with a 32-byte key, 12-byte nonce, and full 16-byte tag. The kernel provider is `rfc7539(chacha20,poly1305)` with 12-byte IV and 16-byte auth size; the ESP variant and its different IV conventions are not used. The provider must match the primitive's known-answer vectors. [RFC 8439](https://www.rfc-editor.org/rfc/rfc8439.html), [kernel AEAD API](https://docs.kernel.org/crypto/api-aead.html)

## 5. Establishment, confirmation, and duplicate handling

### 5.1 Encrypted control subtypes

| Subtype | Name | Payload bytes | Payload |
|---|---|---|---|
| 1 | IPV4 | 20..1428 at default P | One validated inner packet/fragment |
| 2 | CONFIRM | 32 | New full handshake hash |
| 3 | READY | 32 | Same handshake hash |
| 4 | COMMIT | 32 | Same handshake hash |
| 5 | COMMITTED | 32 | Same handshake hash |
| 6 | PING | 16 | Fresh random challenge |
| 7 | PONG | 16 | Echo of a live outstanding challenge |
| 8 | REKEY_START | 48 | Attempt ID then parent full handshake hash |
| 9 | REKEY_OK | 48 | Same attempt and parent |
| 10 | REKEY_NEEDED | 32 | Current parent full handshake hash |

All state-changing controls are accepted only for their expected generation, role, state, and correlation value. A valid tag alone does not authorize an unexpected transition. Control records never enter the inner IP stack.

### 5.2 Activation exchange

1. I sends INIT; R verifies IK message 1, the pinned identity, payload, and current authorization. R creates one candidate, prepares its directional keys, and sends RESPONSE. I verifies message 2 and installs its candidate. Neither side delivers candidate application data.
2. I sends CONFIRM under the candidate TX key. R verifies the full hash and sends READY under its candidate TX key. This demonstrates possession of the completed exchange's directional material before application activation.
3. On READY, I emits COMMIT and commits locally: candidate becomes current, old TX stops, and flow continuity is applied as specified below. R remains prepared to receive candidate control; candidate application data arriving before its COMMIT is processed is dropped.
4. R validates COMMIT and commits locally, then sends COMMITTED. I records successful completion on COMMITTED. Both sides now use the new generation; R's new TX begins only after I has installed/confirmed that generation.

This is not a simultaneous distributed commit. Reordering/loss can temporarily drop data, which inner TCP can retransmit. Each control retry is a new authenticated record with a **new counter** and the same correlation payload. Handlers are idempotent: repeated CONFIRM, READY, COMMIT, and COMMITTED can produce the expected response without a second installation, counter reset, or flow reset. Older requests never roll the state backward.

The whole attempt has a 10-second deadline. Failure before local commit destroys the candidate and retains an old generation only if it is still valid. Failure after local commit cannot roll TX back to the old key: the relationship becomes unavailable and requires fresh establishment. A lost COMMITTED is retried through repeated COMMIT; the responder responds from its already current generation. It does not commit twice.

### 5.3 Handshake duplicates and replay

INIT retransmissions reuse the exact previously serialized bytes; they do not encrypt a different payload with an existing Noise handshake nonce. R caches the exact INIT and RESPONSE during the bounded attempt. An identical duplicate gets the same cached RESPONSE. The Noise state machine is not advanced twice. A conflicting INIT does not replace an existing authorized pending attempt.

After successful activation, retain a nonsecret completed-attempt/response cache for 20 seconds, within the per-peer control budget. Later replay of an old INIT may at most create one fresh, rate-limited candidate; it cannot install old keys, evict the active session, preserve flow state, or become active without new-key confirmation. Every candidate response uses fresh responder ephemeral state and a new locator. No unauthenticated CLOSE/reset exists.

The 252-byte INIT receives a 204-byte RESPONSE, so that exchange is not a large reflection amplifier. Known-endpoint admission and rate limits still apply because source addresses can be spoofed. No automatic cookie challenge or group discovery service is included.

## 6. Rekey, continuity, and recovery

Soft key limits trigger a complete fresh-ephemeral IK exchange. The designated initiator coordinates it. A responder nearing its own limits sends REKEY_NEEDED under the current key. A malicious/repeated request cannot extend limits or create parallel replacement exchanges.

To retain flow permission, I first sends REKEY_START under the current generation, binding a newly chosen attempt ID to the old full handshake hash. R verifies that old generation/current incarnation, records a single continuity grant, and returns REKEY_OK under the old key. Only then may the matching mode-1 INIT proceed. Both still require a new complete IK exchange and confirmation. The grant expires with the attempt or old generation and cannot be reused for another attempt.

An unused grant expires ten seconds after its first REKEY_START, with no extension from duplicates. A matching INIT arriving within that time consumes it for that candidate; the candidate then has the ordinary ten-second exchange deadline. A parent generation reaching hard expiry before commit invalidates the continuity attempt. Thus preparation cannot indefinitely reserve a replacement slot or preserve flows across expired parent keys.

If the old session is lost/expired, an incarnation changed, the grant is absent, or the parent differs, a mode-1 attempt fails; it is not silently treated as a flow-preserving rekey. The initiator can make a new mode-0 attempt after abort/backoff. Mode-0 commit clears both sites' affected flow-admission state, changes the local conntrack zone/incarnation, and rejects all old-generation data. Applications must reconnect.

For a successful rekey, the local continuity context and conntrack zone remain unchanged. Old TX stops at local commit. One old RX generation remains for at most 15 seconds and never beyond its hard lifetime. It retains its own replay bitmap and is still subject to **current** service policy. A second rekey first retires an older retained RX or waits within the current key's valid limits; it never grows the generation count.

Rekeying does not revive permission removed by a service-rule change. Revocation blocks current, pending, and retiring generations at once locally; late Netlink completions cannot reinstall them. Pre-authorized packets already beyond the final admission boundary can still arrive afterward and must be distinguished in evidence.

## 7. Replay, sequencing, and key-use limits

The receive replay window is **1024 counters** per direction/generation. Bit zero represents the highest accepted counter. Packets ahead of the window may shift it only after successful authentication; authenticated but already accepted counters and counters more than 1023 behind the highest are dropped. Initial state has no accepted counter, not an implicitly accepted zero.

Two concurrent decryptions can pass a cheap preliminary check, but acceptance performs a second test and bitmap update under the same per-generation replay lock. Only one can claim a counter. Authentication failure never updates this bitmap. After successful authentication, a structurally invalid or ineligible record consumes its counter and is dropped rather than being processed repeatedly.

Key age is measured using suspend-inclusive monotonic boot time. The earlier of the following limits applies independently to TX and RX, counting **all** protected control/data records:

| Quantity | Soft limit | Hard limit |
|---|---|---|
| Age from candidate key installation | 120 seconds | 180 seconds |
| Records per direction | 524,288 | 1,048,576 |
| Authenticated plaintext bytes per direction, including the 4-byte record prefix | 512 MiB | 1 GiB |

TX reserves record/byte allowance before encrypting. Gaps and failed outputs do not refund counters. RX commits accepted-record/byte charges with replay acceptance. A record that would exceed a hard bound is not delivered. These deliberately conservative project limits are operational choices, not calculated guarantees of immunity to every cryptographic attack. Tag failures are rate-limited and counted but do not independently revoke trusted state.

Sleep/resume, snapshot restoration, and cloning of live state do not preserve active eligibility. Supported environments must prevent these transitions while active or invalidate keys/flows before forwarding resumes, using fresh randomness for recovery. A wall-clock change alone does not reset a key age or counter.

## 8. Liveness and controller supervision

Each active relationship sends PING every five seconds, including when application traffic is busy. A challenge is unique, outstanding for at most 15 seconds, and accepted PONG must match one of at most three live challenges. Repeated PONG for an already consumed challenge does not refresh health. No authenticated round-trip success for **15 seconds** closes application eligibility, clears flow continuity, and requires fresh recovery. Inbound application data alone cannot hide a one-way failure.

The correlated activation exchange seeds this clock: I's valid READY proves its CONFIRM round trip, R's valid COMMIT proves its READY round trip, and COMMITTED refreshes I's completion evidence. Thus first activation does not start with an uninitialized or already-expired liveness timestamp. Thereafter current-generation challenge/PONG exchanges refresh health. A retiring generation admits only still-eligible in-flight IPv4 records, not liveness or state-changing control, so old traffic cannot indefinitely sustain the replacement's health.

The controller renews its kernel lease once per second. A lease is valid for **five seconds** after the last accepted renewal. Every final packet-admission/key-install operation checks current boot time against that lease; it does not wait for a cleanup timer to run. Thus no new admission occurs at/after expiry even if cleanup work is delayed. Packets admitted before expiry can remain in downstream queues. A CLI exiting is not a lost controller lease.

Handshake/control retries use intervals of 0.5, 1, 2, and 4 seconds, with ±10% independent jitter, subject to the attempt's overall 10-second deadline. A failed attempt backs off 1, 2, 4, 8, 16, then 30 seconds, with the same jitter; success resets backoff. The first currently required attempt is immediate. Enabled peers may continue bounded recovery attempts indefinitely, but there is never an unbounded retry queue or more than one candidate per peer.

## 9. Kernel architecture and integration points

The selected kernel component is an external GPL-compatible module using public/exported facilities of Linux 6.18.y. Each gateway namespace owns one unnumbered `ARPHRD_NONE`/point-to-point Layer-3 `vpn0`, one IPv4 UDP tunnel socket, one controller lease, and separate peer/policy tables. The interface carries IPv4 packets without Ethernet headers. It is not a TUN file descriptor read by userspace.

The socket uses the IPv4 UDP-tunnel API with an encapsulation receive callback and binds address/interface/port as specified above. The callback parses bounded datagrams, queues authenticated-record work to the kernel data workers, and delivers only INIT/RESPONSE exchanges to the authorized userspace controller through bounded private events. Userspace returns handshake output as a transmit request; the kernel's own socket applies rate, attempt, and endpoint rules. Userspace never relays normal application records. [Linux 6.18 UDP-tunnel API](https://raw.githubusercontent.com/torvalds/linux/v6.18/include/net/udp_tunnel.h)

The virtual device's transmit callback accepts an owned packet once, validates its authorization provenance and current peer state, and queues kernel encryption/output. Accepted/decrypted inner packets are reinjected once on `vpn0` with their authenticated peer context. Ordinary Linux IP forwarding performs the two real gateway hops and checksum changes. Decapsulation does not manually consume an extra inner TTL. [Network-device lifetime rules](https://docs.kernel.org/networking/netdevices.html)

### 9.1 Netfilter ordering

Scope update, 4 November 2026: the persistent HR1 perimeter, packet marks and trusted packet context are scrapped. The table records the implemented socket/device path and the retained HR4 proposal; it does not claim the old project zone or FORWARD hooks exist. See [the scope decisions](docs/PERIMETER_REMOVAL.md).

| Hook / boundary | Owner and responsibility / implementation status |
|---|---|
| PRE_ROUTING, −450 | HR4 #41 outer profile guard: reject UDP fragments addressed to the configured underlay IP/interface before shared defrag; reject unsupported project-port options/lengths. No mark clearing/minting, LAN perimeter or authorization. |
| PRE_ROUTING, −400 / −200 | Standard Linux defragmentation/conntrack when enabled. Project inner normalization and zone assignment have no live adapter; the fragment/zone/witness helpers remain library code. No project −300 hook is installed. |
| FORWARD, 0 | Gateway ruleset (`vpt-gateway.sh`): forward only LAN↔`vpn0` for the peers' prefixes; other forwarding, including underlay↔LAN, is dropped. There is no project −50 FORWARD hook. |
| `vpn0` transmit callback | Destination ownership, current peer/lease/generation and kernel service ACL admission before protected output. The live thin path uses stateless ACL admission; strict witness integration remains a separate qualification gate. |
| Bound UDP socket receive callback | Exact underlay binding, endpoint/locator, AEAD/replay, inner IP/profile and kernel ACL checks before reinjection on `vpn0`. Ordinary Linux forwarding consumes the second gateway hop. |
| LOCAL_OUT / POST_ROUTING | Kernel tunnel socket output and normal checksum/routing/conntrack confirmation. No project mark classification, local-error zone hook, inner/outer NAT or generic ICMP-error bypass. Associated errors require separately reviewed implementation/qualification. |

The −450 guard uses Linux's `NF_IP_PRI_RAW_BEFORE_DEFRAG`, before shared defragmentation at −400. It does not replace a forwarding firewall. [Linux 6.18 priority definitions](https://raw.githubusercontent.com/torvalds/linux/v6.18/include/uapi/linux/netfilter_ipv4.h)

IPv6 forwarding is denied by the gateway ruleset independently of these IPv4 checks. Local ARP and required Wi-Fi link control stay local. Host Ethernet addresses are not exported over the tunnel, and the AP is not a LAN bridge.

### 9.2 Packet identity and held references

No packet-mark encoding is reserved, minted or trusted by this scope. Authentication comes from the accepted protected record and its held peer/generation references. Kernel admission rechecks the current namespace/peer incarnation, interface direction, live lease/session and current ACL under its shared admission lock. The small forwarding ruleset selects interfaces and prefixes; it cannot manufacture this authorization.

Zone and witness libraries remain available for reviewed future integration, without reviving the removed HR1 context or mark scheme. Strict live witness/conntrack association, complete inner normalization, associated-error contexts and retained output-fragment references are still unimplemented qualification gates. Queued work must retain the required counted references and IDs, never raw cfg/ACL pointers beyond an RCU read section; a reused slot number alone cannot identify a flow. These lifetime requirements do not claim that the current thin path already has such adapters.

The driver advertises no tunnel GSO/GRO, hardware checksum, or scatter/gather offload capability in profile 1. Project-bound aggregated skbs are software-segmented into individual IP packets before strict per-packet length/checksum/policy checks, not merely after a large aggregate has already been mistaken for malformed input. Segmentation accepts at most 64 resulting packets per aggregate and charges their memory/queue costs; excess work is dropped with accounting. Checksum completion uses the stack's offload metadata correctly before record serialization. A multi-packet skb is never encrypted as one oversized record. Ordinary output fragments retain the admitted datagram's conntrack provenance. Initial namespace captures additionally disable aggregation where configurable; physical NIC offloads are recorded rather than mistaken for wire behavior.

## 10. Authoritative transmit and receive order

The normalization, live witness/zone, bounded-worker and associated-error steps
below remain integration acceptance targets. The implemented socket/device
boundaries and their current limitations are in section 9.1. No future step may
reintroduce the scrapped HR1 mark or trusted-context scheme without a new scope
decision; the retained zone/witness libraries alone do not install these hooks.

### 10.1 LAN-to-tunnel transmit

1. Ingress classification validates the source as a non-gateway unicast address on that site's LAN and the destination as a declared remote prefix. Unsupported options/families/protocols are dropped. Any fragments are normalized in the intended peer's ingress context.
2. The normalized packet receives the relationship's current conntrack zone. Conntrack validates supported transport state/checksums. No NAT or automatic helper may modify this classification.
3. Routing selects `vpn0` for the remote prefix. The FORWARD gate checks current ACL, permitted initiation/reply direction, and the flow witness described in Section 11. It admits only eligible packets; no packet mark is assigned. Linux forwarding handles TTL and any permissible inner fragmentation for the virtual interface's MTU.
4. `vpn0` transmit checks the held peer against destination ownership, the conntrack/witness identity, current incarnation, and current policy. Non-initial output fragments use the retained admitted-flow provenance rather than guessing a missing TCP/UDP port. A locally generated associated error instead holds its validated recent-packet/error context; it does not require an application witness that may not yet exist. That exception cannot create application permission.
5. A bounded queue owns the skb and references. A worker reserves a unique counter and key allowance, completes AEAD with the exact header as AAD, and checks eligibility/current policy again before final output admission. A failed check discards the work; its reserved counter is not refunded.
6. The outer route is resolved only in the configured underlay namespace/interface using the gateway outer source. IPv4 DF is set and a nonzero UDP checksum is produced. The packet is handed to UDP/IP output, which is the final tunnel-owned emission boundary.

No unconfirmed handshake triggers temporary plaintext forwarding. A SYN arriving while protection is unavailable may be dropped; its ordinary TCP retransmission can succeed after activation.

### 10.2 Tunnel-to-LAN receive

1. The underlay gate admits only the configured endpoint tuple to the tunnel socket, rejects unsupported outer input, and applies packet/byte budgets. Invalid framing is discarded before allocating a large object or invoking a handshake.
2. A RECORD locator identifies a live pending/current/retiring generation in the receiving namespace. Endpoint, role, age, counter range, and preliminary replay checks must pass. Counter values ≥1,048,576 are invalid in this profile, even if a malicious key holder could create their tags.
3. AEAD verifies the exact AAD/ciphertext/tag using the candidate direction's key and nonce. Failure discards all tentative plaintext without replay, flow, or liveness advancement.
4. Under the generation replay lock, recheck validity and claim the counter at most once. A valid control is passed only to its state-specific handler. Candidate application data is dropped until the local COMMIT condition is met.
5. Validate the complete inner representation and source ownership against the **authenticated** peer before entering shared IP processing. Recheck held peer eligibility and inject it on `vpn0` without minting a packet mark. Source validation is not deferred until after a fragment or conntrack association has already been created.
6. The early inner hook performs peer-scoped normalization, then selects the current conntrack zone. Conntrack and the project FORWARD gate independently enforce transport validity, current flow permission, and local service-LAN destination.
7. The final LAN-delivery authorization rechecks lease, generation/continuity validity, and current ACL under the peer admission lock, then hands the packet to normal LAN output. It cannot route it to another peer, the WLAN, or a local management listener. Ordinary LAN ARP and Host B's own firewall complete delivery.

Counters distinguish authenticated record receipt from actual LAN delivery; successful decryption alone is not service acceptance. A packet may consume a replay counter yet subsequently fail policy.

### 10.3 Locally generated errors

A valid error may be required before a complete TCP flow exists—for example, an initial permitted SYN with DF set is too large, or TTL expires at a gateway. The early gate retains a bounded recent-packet context only after source and current service/initiation eligibility are checked. LOCAL_OUT admits an error only when its quoted tuple, IP identity/length, and available transport prefix match that context/current policy. TTL and IP-header checksum fields that legitimately change in transit are not compared as immutable identifiers.

The inner source of a gateway-generated remote error is its LAN address (`10.77.1.1`, `10.77.2.1`, or `10.77.3.1`). It therefore passes peer site ownership. This special associated-error class does not permit unrelated packets from that gateway or unrestricted ICMP. Supported errors are relevant destination-unreachable and time-exceeded messages. Redirect and source-quench messages never update policy/routes. Diagnostic echo uses an explicit echo association, not this error exception.

Outer ICMP errors are checked against recent outer transmissions. A short quotation containing only the outer IP and UDP headers can establish a plausible path event but may be ambiguous across many records. It may suspend/revalidate a path; it is not used to invent an arbitrary inner quotation or claim peer authentication. An inner fragmentation-needed error is generated only when the original inner packet is available and unambiguously associated, such as a local oversize send. Source/destination endpoint matching alone does not cryptographically authenticate ICMP.

## 11. Flow policy, conntrack, and zone ownership

The module owns the authoritative ACL and admission witnesses. Linux conntrack owns transport tracking, not permission. A small nftables ruleset only keeps ordinary forwarding from becoming a second path. All three are scoped to the gateway namespace, and none grants access merely because another component has allocated state.

Each relationship continuity context has a nonzero 16-bit conntrack zone from the module's reserved local zone pool. Both LAN→VPN and VPN→LAN directions select that same local zone before conntrack. Zone numbers need not match at the remote gateway. Fresh establishment assigns a clean zone/incarnation; flow-preserving rekey retains it. A zone is not reused until its old witnesses/queued references are drained and its project conntrack entries are purged. Exhaustion denies new activation instead of reusing uncleared state.

The gateway has `nf_conntrack_tcp_loose=0`, conservative TCP validation, checksum checking, no conntrack helper expectations, and no flow-table/hardware fast-path bypass. These settings are checked in the environment manifest. The design does not rely on conntrack's `ESTABLISHED` label alone because transport tracking and administrative authorization have different lifetimes. [Linux conntrack controls](https://docs.kernel.org/networking/nf_conntrack-sysctl.html)

### 11.1 TCP admission witness

A witness is keyed by peer incarnation/continuity context, zone, original five-tuple, and a held reference to the conntrack object. It contains the matched allow-rule identity, initiator direction, creation/last-use times, phase, and the initial SYN/SYN-ACK sequence correlation. It is created only when a valid initiating SYN (SYN set, ACK clear, no TCP data) passes the current ACL and conntrack validation at the project FORWARD gate.

| Witness phase | Admitted progression |
|---|---|
| SYN_PENDING | Retransmitted matching initial SYN; matching reverse SYN-ACK acknowledging the initial sequence plus one; associated valid errors |
| SYNACK_SEEN | Matching SYN/SYN-ACK retransmissions and the initiator's final ACK with the expected acknowledgment/sequence; that ACK may carry application data |
| ESTABLISHED | Current-policy packets belonging to that connection and accepted by conservative conntrack validation |
| CLOSING | Valid FIN/RST/closing behavior and bounded retransmissions until closing expiry |

Malformed flags, a mismatched handshake correlation, a new reverse SYN, or a tuple seen only through midstream ACK/data cannot create a witness. TCP simultaneous open and SYN-carried TCP Fast Open data are excluded from profile 1; ordinary HTTP/TCP with the standard handshake is supported. The witness is a firewall admission state, not a second TCP reliability implementation. Conntrack still handles the transport's ordinary sequence/window/closing validity.

Deleting a witness immediately removes permission regardless of retained conntrack state. Idle expiry, fresh establishment, or revoked policy cannot reconstruct it from subsequent midstream packets. The two gateways do not synchronize witness tables; each must see its own valid initiation and replies. Lost state at either site can require application reconnection even if the other site remembers the old tuple.

### 11.2 UDP and ICMP

An explicit UDP rule creates an association only in its permitted initiation direction. Its reverse five-tuple is eligible for 30 seconds since valid activity, provided the rule is still present. This is firewall state, not authentication of the remote application. No automatic helper opens additional ports. An explicit diagnostic echo rule creates a five-second association scoped to source, destination, identifier, and outstanding sequence; arbitrary reply IDs cannot refresh it.

The production demonstration policy is TCP/8080 only. UDP and echo rules are separately selected diagnostic fixtures, removed after their tests. Necessary associated errors remain available under their narrow rules.

## 12. Fragment normalization and IP correctness

The module owns protected-inner reassembly instead of assigning peer security context to an uncontrolled shared fragment cache. An early PRE_ROUTING hook at −450 consumes those fragments before Linux conntrack's −400 defragmentation. All bytes are authenticated/source-checked first on tunnel ingress; LAN ingress has its own validated local source/interface context. A complete normalized packet resumes once through the ordinary IP path with fragment fields cleared and a recomputed header checksum, without an extra TTL decrement.

A set is keyed by namespace incarnation, local ingress role, authenticated/intended peer continuity context, source, destination, IP protocol, and IPv4 identification. It holds a reference to that context. IPv4 options are rejected. Fragment offsets, eight-byte alignment where required, final length, address/protocol and TTL/DSCP consistency must pass. Any overlap, including an ambiguous duplicate fragment admitted under a different record counter, rejects the set. Last-fragment length cannot exceed the IPv4 65,535-byte total limit.

ECN is merged, not required to be identical across fragments: a legitimate outer congestion mark may affect only one fragment. Use Linux 6.18's `ip_frag_ecn_table` semantics: reject a mixture of Not-ECT with any ECN-capable/CE mark; if any valid fragment is CE, the reassembled datagram is CE; otherwise preserve the first fragment's ECN value. This prevents either erasing congestion evidence or rejecting a valid CE/ECT combination merely for differing bits. [Linux fragment ECN table](https://raw.githubusercontent.com/torvalds/linux/v6.18/net/ipv4/inet_fragment.c)

At most 16 sets, 64 fragments per set, 256 KiB of retained payload and 512 KiB of charged fragment memory are allowed per peer. A set expires five seconds after its first accepted fragment; later fragments do not extend that deadline. Complete packets undergo exact length/checksum and current transport-policy validation. Pool limits can reject pathological but syntactically valid extremely fragmented traffic; this is an explicit supported-envelope limit, not a general all-IPv4 forwarding claim.

For DF-clear oversized traffic, ordinary IP output fragments the admitted datagram into inner fragments that fit `vpn0`; each gets its own protected record/counter. The fragments retain the held conntrack/admission provenance at transmit. On the remote gateway they are normalized before service-policy inspection. DF-set oversized traffic is dropped with an associated fragmentation-needed error when normal ICMP rules permit. DF is not cleared to conceal a size failure.

The reassembly cache is flushed for an affected fresh session, peer revocation, or namespace removal. Fragments crossing an ordinary rekey may share the same preserved continuity context, but must still belong to a valid current/retiring record generation and pass current policy. A stale fragment set cannot survive a new peer incarnation.

## 13. Policy transactions and persistent failure guards

An immutable kernel configuration snapshot contains peer identity pins, endpoint/interface bindings, unique source/destination ownership, allow rules, pair PMTUs, and operating profile 1. Readers use RCU plus a held peer/incarnation reference. A writer validates a complete replacement before publishing it. For the commit cut, it takes affected peer admission locks in ascending slot order, publishes the snapshot/state change, and releases them before reclamation or other blocking work. Final admission reloads the current snapshot while holding that peer's lock; a reader cannot finalize a stale permission solely because it entered an earlier RCU read section. Unknown attributes and unsupported extensions fail the transaction.

A service-rule update atomically publishes the new ACL, then invalidates only witnesses/reassembly context affected by the changed permission. Every admission rechecks the **current** rule, so a cached witness cannot bypass an intervening tightening. A witness retains its stable rule ID, not an unprotected pointer into a reclaimed old snapshot. Unchanged rules retain valid witnesses; removing B2 access must not force unrelated B flows to reconnect. When an old snapshot is found on queued work, that work is re-evaluated against the current snapshot rather than automatically inheriting its old permission.

Identity/endpoint/prefix reassignment is disruptive: block the affected relationship, invalidate all key generations and witnesses, replace the validated configuration, and require fresh establishment. A malformed replacement leaves the old valid configuration active with an explicit failure result. A standalone DISABLE/REVOKE operation does not depend on constructing a valid replacement and must always be able to close eligibility.

The peer admission lock serializes the final authorization boundary with revocation/lease expiry/commit. No crypto operation, sleeping allocation, or work-queue drain runs while that lock is held. A packet authorized and handed to downstream UDP/LAN output before a policy commit may arrive later; it is not retrospectively recalled. Work still owned by the tunnel when the relationship closes is dropped.

### 13.1 Gateway forwarding ruleset

*Scope change, 3 November 2026: the persistent nftables perimeter with packet marks and trusted packet context was scrapped
([PERIMETER_REMOVAL.md](docs/PERIMETER_REMOVAL.md)). The kernel's own ACL is the authorization; this ruleset only keeps ordinary
forwarding from becoming a second path.*

`infra/gateway/vpt-gateway.sh` installs one `forward` chain with policy drop and two accepts: LAN to `vpn0` for the peers'
destination prefixes, and `vpn0` to LAN for their source prefixes. Anything else, including LAN to underlay, is dropped. `down` removes
`vpn0`, the routes and the MSS clamp and leaves the ruleset with no accepts. There are no marks, no zones and no claim that the rules
survive the module or controller being absent beyond that; local LAN/console administration, NAT and IPv6 are outside this ruleset.

Where routes and firewall state require separate operating-system transactions, the affected relationship remains blocked until all components have been verified, then the kernel snapshot enables it. nftables batch updates are atomic within their own transaction; the design does not pretend that route, Netlink, and firewall changes constitute one native global transaction.

## 14. Object ownership, concurrency, and teardown

| State/object | Authoritative owner | Lifetime and invalidation |
|---|---|---|
| Namespace/gateway context | Per-net module instance | Holds gateway tables/limits; explicit removal closes eligibility before resource destruction |
| Identity and Noise exchange | Trusted userspace controller | Pins immutable peer identity; only bounded handshake work; no data-path nonce ownership |
| Peer context | Kernel gateway instance | Config/incarnation, prefix ownership, lease and lifecycle; held references for queued work |
| Key generation | Kernel peer context | Separate immutable keys, monotonic counters, replay bitmap, limits; erase only after disabled references drain |
| ACL snapshot | Kernel config transaction | RCU publication; current policy checked at final admission |
| Admission witness | Kernel policy layer | Holds conntrack reference and rule/context; revocation/expiry removes permission immediately |
| Conntrack entry | Linux conntrack in selected zone | Transport state only; retained state is harmless without current witness/context |
| Fragment set | Kernel normalization layer | Context-scoped, charged allocation, fixed first-fragment deadline |
| Queued skb/crypto request | Queue/worker currently owning it | Exactly one owner; holds peer/generation/witness references; revalidated before output |
| Routes and forwarding ruleset | Trusted administrator (`vpt-gateway.sh`) | `down` leaves a ruleset that forwards nothing; errors leave forwarding blocked |

TX counter reservations use one atomic counter per generation/direction. Replay acceptance uses its own short lock. Per-peer final-admission/state transitions use the peer lock; configuration writers are serialized and publish RCU snapshots. Operations never wait for userspace or crypto while holding packet locks. Each asynchronous completion verifies peer incarnation and configuration/attempt applicability before changing state.

Where nested acquisition is unavoidable, the order is configuration-writer serialization, peer locks in ascending slot order, generation replay/accounting lock, then witness/fragment bucket lock. RX ordinarily releases the replay lock before later policy admission, avoiding an inverse replay→peer dependency. RCU synchronization and worker drains occur outside these packet locks.

The kernel uses four bounded packet workers per gateway, with at most two simultaneous crypto requests for one peer. TX output completion is released in counter-reservation order for that peer/generation, including failed-operation slots. RX replay acceptance is released in the peer's dequeue order; a completed later decrypt waits for earlier in-flight work within the charged queue bound. Thus a slow crypto request cannot allow an unbounded run of later worker completions to age out a still-pending record. Network reordering beyond the 1024-record window remains the declared loss tradeoff. Scheduling is byte-fair deficit round robin with a 4096-byte quantum per active peer. Control is serviced ahead of data in batches of at most four, then data gets a turn. Shared CPU/link exhaustion can still impair all peers; this is not a hard CPU reservation.

Namespace/peer removal first closes admission, detaches new work, and invalidates generations/witnesses. It then cancels timers/work, drains references, releases socket/device resources, and finally erases secrets/frees objects. Socket/device/Netlink references cannot become a permanent cycle retaining an otherwise removed gateway. A namespace name disappearing is not proof that its retained objects were destroyed. A recreated name always gets a new incarnation. The reference/RCU rules must hold for normal removal, Netlink owner loss, per-net exit, and whole-module unload. [External module and build compatibility](https://docs.kernel.org/kbuild/modules.html)

## 15. MTU, exact overhead, and congestion markings

### 15.1 Byte budget

| Data component | Bytes |
|---|---|
| Outer IPv4, no options | 20 |
| UDP | 8 |
| VPT1 common envelope | 16 |
| Transmitted counter | 8 |
| Encrypted subtype/flags/payload-length prefix | 4 |
| AEAD authentication tag | 16 |
| Total expansion above inner IP packet | **72** |

For inner length `L`, the protected UDP payload is `L + 44` and outer IPv4 total is `L + 72`. With default `P = 1500`, the maximum inner packet is **1428**. Inner IPv4/TCP with no options permits at most 1388 TCP payload bytes; inner IPv4/UDP permits 1400 UDP application bytes. TCP options further reduce its payload. Link-layer headers, Wi-Fi retransmissions, airtime, and preambles are outside this IP budget and are included separately in performance explanations.

There is no optional padding, compression, or hidden native alignment. A full-size data record's common `body_length` is 1456; its UDP payload is 1472. The largest defined encrypted control is REKEY_START/OK at 120 outer bytes. INIT, at 252 outer bytes, is the largest non-data message. Thus the recovery exchange fits well below the admitted minimum path budget.

### 15.2 Path and interface policy

Operating profile 1 admits administrator-qualified bidirectional `P` in **1280..1500 bytes**, default 1500. Both endpoints configure the same pair value and validate it during establishment; no peer may propose an unconfigured downgrade. Pair RX cap is `P − 72`, range 1208..1428.

A gateway's shared `vpn0` transmit MTU is the minimum pair cap across all peers present in its committed configuration, including those not activated yet. With no configured peer it is 1428. Adding a smaller-budget peer lowers it in the gated configuration transaction before that peer activates. The kernel computes this from the staged snapshot at COMMIT under RTNL before publishing it; manual MTU changes are refused. Its receive path may still admit a valid incoming packet up to that relationship's pair cap, even if another peer lowered the shared **transmit** MTU. This distinction avoids incorrectly rejecting a larger but valid A–B input just because B also serves a smaller-budget C path. The static P1280 bridge and local DF-set PTB campaign is separate from routed-path/outer-ICMP and DF-clear qualification.

The default 1500 profile is appropriate only after the Ethernet cable and virtual underlay paths (and each Wi-Fi LAN) have been verified. An interface's configured MTU alone is insufficient evidence for an intervening path. A credible local oversize error or validated path-size indication suspends that pair until the configured budget is revalidated and, if changed, freshly established. Unrelated peers remain eligible.

This is a static-path profile. If large packets silently disappear while small control packets still work, it may remain apparently live until diagnosis establishes the unsafe budget. No automatic DPLPMTUD or universal black-hole detection is claimed. There is no outer fragmentation fallback and no MSS rewriting to hide a path failure. Proper inner fragmentation-needed feedback is the normal TCP recovery mechanism.

### 15.3 DSCP, ECN, and reliability

Outer DSCP is best effort. For data, outer ECN copies the authenticated inner packet's two ECN bits (normal encapsulation mode). On receive, apply RFC 6040's default egress rule: inner Not-ECT stays Not-ECT unless outer CE requires dropping; otherwise preserve the more severe mark under the ordering CE, ECT(1), ECT(0), Not-ECT. Update the inner IPv4 checksum if its ECN changes. Control/handshake traffic is outer Not-ECT. Anomalous combinations are counted with bounded diagnostics. [RFC 6040](https://www.rfc-editor.org/rfc/rfc6040.html)

The tunnel adds neither data retransmission nor a second reliable stream/congestion window. Inner TCP handles its congestion and retransmission; explicitly permitted UDP remains rate-controlled by the admitted application/test fixture. Tunnel control retransmissions and packet bursts have their own caps. This profile is confined to the administered lab. A future public-network profile must separately justify NAT behavior, control overhead, and any nonresponsive inner traffic. [UDP tunnel guidance](https://www.rfc-editor.org/rfc/rfc8085.html#section-3.1.11)

## 16. Concrete operating limits

These are selected admission bounds and qualification targets, not measured capacity promises. Raising a bound changes the operating profile and requires affected resource/security qualification. Reaching a limit drops/denies new work with a reason counter; it never opens a stateless forwarding fallback.

### 16.1 Scale, queues, and allocation

| Resource | Profile 1 bound |
|---|---|
| Remote peers | 4 per gateway; at least 2 concurrently demonstrated at B for M |
| Gateway instances in initial namespace host | 8 |
| Owned remote prefixes | 4 per peer, 16 per gateway; non-overlapping and non-default |
| Explicit ACL rules | 128 per gateway |
| Pending exchanges | 1 per peer, 4 per gateway |
| Generations per relationship | Current + 1 retiring RX + 1 pending candidate |
| Replay bitmap | 1024 bits per receiving generation |
| TX queue | 128 packets and 512 KiB charged skb memory per peer; both limits apply |
| RX crypto queue | 128 packets and 512 KiB charged skb memory per peer |
| Control/event queue | 32 messages and 64 KiB per peer, including cached exchange output |
| Admission witnesses | 1024 per peer, 4096 per gateway; maximum charged witness object 256 bytes |
| Project conntrack entries | 8192 per gateway across current/retired project zones; exhausted space blocks new project admissions |
| Conntrack hash admission ceiling | 32,768 entries for the isolated qualification kernel; actual sysctl scope/host coexistence is verified, not assumed namespace-private |
| Reassembly | 16 sets, 64 fragments/set, 256 KiB payload and 512 KiB charged memory per peer |
| Recent inner error contexts | 256 per peer, 128 bytes maximum each, 10-second retention |
| Recent outer packet contexts | 256 per peer, 128 bytes maximum each, 10-second retention |
| Live path challenges | 3 per peer |
| Kernel packet workers | 4 per gateway, no more than 2 in-flight crypto requests per peer |
| Module-owned charged memory | 16 MiB per gateway; 128 MiB aggregate for the eight-gateway namespace host |
| Controller process memory | 64 MiB per gateway, enforced by the host's process/cgroup policy rather than by network namespaces alone |
| Retired ACL snapshots awaiting reclamation | 2; additional publication waits/fails busy rather than allocating indefinitely |

Byte limits charge actual skb `truesize`, fragment metadata/bitmaps, crypto-request buffers, context objects, and retained references—not merely payload lengths. The kernel-wide OS/IP/conntrack allocation and userspace controller memory are reported separately; “16 MiB” is not a claim about the whole machine's memory use.

At four peers, the nominal charged TX+RX+fragment ceilings total 6 MiB; witness ceilings add 1 MiB, and recent contexts/control/keys add bounded overhead. The 16 MiB allocation gate still checks every allocation against measured object sizes. It is authoritative if an actual provider or allocator requires more than the nominal estimate. The eight-instance host budget is at most eight such reservations. No platform is qualified merely because this arithmetic fits.

### 16.2 Timers and rates

| Item | Value / consequence |
|---|---|
| Whole establishment/rekey activation attempt | 10 seconds from first INIT; no phase/duplicate extends the deadline |
| Completed exchange duplicate cache | 20 seconds; nonsecret bounded cache only |
| Rekey soft/hard age | 120 / 180 seconds from candidate installation |
| Rekey soft/hard records | 524,288 / 1,048,576 per direction, including control |
| Rekey soft/hard bytes | 512 MiB / 1 GiB authenticated plaintext per direction |
| Retiring RX overlap | 15 seconds maximum and no later than old hard expiry |
| Liveness PING interval / deadline | 5 / 15 seconds |
| Controller renewal / lease | 1 / 5 seconds |
| TCP half-open witness | 15 seconds from initial SYN; retransmissions do not extend it indefinitely |
| Established TCP idle | 120 seconds since valid permitted activity |
| Closing TCP retention | 30 seconds maximum |
| UDP idle association | 30 seconds |
| Echo association | 5 seconds |
| Fragment-set lifetime | 5 seconds from first accepted fragment |
| New exchange admission | 1/second, burst 2 per peer; 4/second, burst 4 per gateway |
| New flow admissions | 100/second, burst 200 per peer; 300/second, burst 600 per gateway |
| Record crypto work | 20,000 records/second, burst 1024 per peer; 50,000/second, burst 2048 per gateway; both valid and invalid attempts count |
| Protected control emission | 20/second, burst 32 per peer; activation/rekey/liveness together |
| Generated ICMP errors | 10/second, burst 20 per peer; 20/second, burst 40 per gateway |
| Detailed log messages | 10/second maximum per gateway; suppression counters retained |
| Routine aggregate status interval | 1 second |
| Diagnostic rotating logs | 10 MiB/file, two retained files per gateway |
| Qualification fixture's uncontrolled UDP offer | Maximum 10 Mbit/s per peer, 20 Mbit/s aggregate; never auto-enabled as an unrestricted service |

Rates use token buckets and byte-fair scheduling; a high configured packet rate does not promise the hardware can achieve it. Admission of required control traffic uses a reserved queue/rate share so a peer's bulk data queue cannot consume all of its control slots. The kernel still cannot guarantee availability against full RF/link/CPU saturation.

An enabled peer that fails to establish keeps only one attempt plus bounded retry metadata. Timer callbacks perform cleanup, but current-time checks at packet/key admission enforce expiration even if a timer callback is late. A half-open/idle flow expiring requires a new permitted initiation. This intentionally limits idle HTTP connection reuse and must be reflected in application tests.

## 17. Local control interface and environment contract

The kernel control family is `vptunnel`, ABI version 1. Requests are privileged unicast Generic Netlink messages authorized in the owning network namespace with `CAP_NET_ADMIN`; cross-namespace effects require explicit host-level administration. One long-running controller owns a gateway, identified by a random 128-bit controller incarnation plus the bound Netlink ownership relationship. PID/port-number reuse is not ownership continuity.

| Request class | Semantic result |
|---|---|
| CLAIM / RENEW | Acquire/renew the five-second lease for a gateway incarnation |
| VALIDATE / COMMIT_CONFIG | Validate a full snapshot, then atomically publish it under its revision; no partial service-rule mutation |
| BEGIN_ATTEMPT | The kernel issues the attempt: a random 128-bit attempt ID (an initiator) and a random, collision-checked receive locator, valid ten seconds, one pending attempt per peer; a responder names the ID of the last INIT the kernel delivered for that peer and receives only a locator |
| STAGE_GENERATION | Install keys/locators/hash as the peer's candidate only for the live authorized pending attempt, once (an identical repeat is acknowledged); mode 1 needs the continuity grant |
| REKEY_REQUEST | Initiator only: ask the responder for a continuity grant for the pending attempt; the answer is the REKEY_GRANTED event |
| EXCHANGE_TX | Emit only the exact eligible INIT/RESPONSE for that attempt through the kernel socket (its attempt ID must be the pending one) |
| CONTROL_INTENT | Request a defined typed control; kernel chooses key/counter and validates state/correlation |
| DISABLE / REVOKE | Close affected eligibility and invalidate current/pending/retiring state without requiring replacement configuration |
| STATUS / COUNTERS | Return bounded nonsecret state with revision and reset epoch |

ABI command IDs are CLAIM=1, RENEW=2, VALIDATE=3, COMMIT_CONFIG=4, STAGE_GENERATION=5, EXCHANGE_TX=6, CONTROL_INTENT=7, DISABLE=8, REVOKE=9, STATUS=10, COUNTERS=11, REMOVE_GATEWAY=12, BEGIN_ATTEMPT=13, and REKEY_REQUEST=14. Private events use command 128 (EXCHANGE_RX), 129 (REKEY_GRANTED) and 130 (REKEY_NEEDED) and never contains data records or keys. Replies echo a 64-bit request ID. Unknown command/attribute IDs are errors, not optional negotiation.

The local attribute schema uses Netlink's native integer types/alignment, distinct from the big-endian on-network protocol:

| Attribute ID | Name / type | Purpose |
|---|---|---|
| 1 | IFINDEX / u32 | Owning gateway VPN interface |
| 2 | CONTROLLER_ID / 16 bytes | Current controller incarnation |
| 3 | CONFIG_REVISION / u64 | Exact current/proposed snapshot revision |
| 4 | PEER_SLOT / u16 | Local slot 1..4 |
| 5 | PEER_INCARNATION / 16 bytes | Kernel-issued peer incarnation, not an arbitrary replacement identity |
| 6 | ATTEMPT_ID / 16 bytes | Exact pending attempt |
| 7 | HANDSHAKE_HASH / 32 bytes | New generation's full session ID |
| 8 / 9 | LOCAL_RX / REMOTE_RX / u64 each | Exact local/remote receive locators |
| 10 / 11 | TX_KEY / RX_KEY / 32 bytes each | Write-only raw Split keys |
| 12 | MODE / u8 | Fresh=0 or granted rekey=1 |
| 13 | PARENT_HASH / 32 bytes | Zero for fresh; exact granted parent for rekey |
| 14 | EXCHANGE_BYTES / binary, ≤224 bytes | Complete eligible INIT/RESPONSE UDP payload |
| 15 | SNAPSHOT / nested, ≤60 KiB | Complete validated configuration described below |
| 16 | INTENT / u8 | A defined encrypted-control subtype, validated against kernel state |
| 17 / 18 | STATUS / COUNTERS / nested | Read-only bounded nonsecret result |
| 19 | REQUEST_ID / u64 | Local request/reply correlation |

The snapshot contains local LAN/underlay ifindexes and addresses, the local 32-byte public identity, fixed profile/suite/port identifiers, and arrays of at most four peers and 128 ACL rules. The local private-identity reference belongs to the controller's protected persistent configuration, never a kernel status value. Each peer entry contains its stable local rule identity, pinned 32-byte public key, endpoint IPv4/port, initiator role, pair PMTU, and up to four non-overlapping authorized remote prefixes. Duplicate remote identities or endpoint tuples within one gateway configuration are rejected. Each ACL entry contains a stable rule ID, peer slot, initiating source/destination prefix, protocol, and destination-port range or explicit diagnostic ICMP echo-request type/code. Prefixes are canonical network address plus length; matching is an explicit union of allow rules with default denial, independent of listing order. Both directions still apply stateful return semantics. Default/overlapping remote ownership and unsupported addresses/types are rejected.

The controller's configuration supplies these values and fixes the following schema rules for profile 1. Each peer carries an explicit `slot` in 1..4, unique within the gateway, which is that peer's `PEER_SLOT` in the snapshot; it is never derived from listing order. The file may be reordered freely, but a slot must not carry another pinned identity, endpoint or owned-prefix set without fresh establishment under a new peer incarnation; this is the identity/endpoint/prefix reassignment that section 13 calls disruptive. "Unsupported addresses" means any prefix, interface subnet or endpoint that overlaps `0.0.0.0/8`, `127.0.0.0/8`, `169.254.0.0/16`, `224.0.0.0/4` or `240.0.0.0/4`; overlap is checked over the whole range, not only the first address, so `64.0.0.0/2` and `192.0.0.0/2` are rejected. The local LAN must contain at least one host other than the gateway (a `/32` LAN is rejected). For an underlay of `/30` or larger, a peer endpoint must not be the underlay network or broadcast address. The LAN and underlay interfaces are distinct physical Linux interface names of 1..15 bytes from `[A-Za-z0-9_.-]`, excluding `.`, `..`, `lo` and `vpn0`.

VALIDATE stages one complete proposed snapshot under its proposed revision; COMMIT_CONFIG names that same revision and request context. A superseded or missing staged snapshot cannot commit. All Netlink messages are limited to 64 KiB including attributes/headers. A schema-invalid message fails as a whole. `STAGE_GENERATION` cannot create its own rekey grant, choose an attempt ID or a locator the kernel did not issue (`BEGIN_ATTEMPT` first; the attempt is consumed by the installation), or change the pinned identity. CONTROL_INTENT cannot request arbitrary ciphertext/plaintext encryption. This fixes the key-installation and policy transaction interface without prescribing source files or a coding sequence.

Malformed lengths/attributes, unsupported ABI versions, unknown request types, stale revisions, and an owner mismatch fail closed. Keys are write-only, omitted from replies and multicast, and not exposed by STATUS. Local administration is trusted; this interface does not protect against a root administrator intentionally changing all firewall rules.

The initial target is a real Linux 6.18.y kernel with namespaces, veth/bridge, Netfilter/nftables, conntrack with zones, IPv4 UDP tunnel support, modules, required Crypto API providers, and matching public build headers/symbol versions. Supported target architectures are x86-64 and ARM64. The physical Pi image must provide its working wireless driver and AP support on the selected kernel family. An external module is built/qualified for each exact kernel build; Linux does not offer a universal stable internal module ABI.

The 31 October platform revision supersedes the earlier 6.12.y baseline. The first inspected deployment candidate is Pi 5 `vpn-b`, Debian 13 (trixie), `6.18.34+rpt-rpi-2712`, AArch64 with 16,384-byte pages and exact matching installed headers. This is an inventory result, not module/radio/runtime qualification. Generic ARM64 VM modules cannot be deployed to this vendor build. Anvit pins the separate 6.18.y reference VM and each Pi's exact image/config/headers/provider/toolchain; patch or vendor changes rerun affected gates. See [Pi 5 platform and migration contract](docs/PLATFORM_PI5.md).

Kernel code uses `PAGE_SIZE`/kernel allocation helpers; userspace queries the runtime page size. Neither may assume 4096-byte pages or a fixed page shift. Recheck actual slab/page/skb/provider charging and layout on both 4 KiB VM and 16 KiB Pi builds without increasing the protocol's memory limits. Resolve crypto providers through the target build's aliases/dependencies; old `chacha_generic`/`poly1305_generic` filenames are not requirements. Continue to allocate `rfc7539(chacha20,poly1305)` with 12-byte IV and 16-byte tag and run the independent AEAD vectors. Changing kernel family does not change VPT1 wire bytes, Noise/Split, Netlink ABI v1, resource limits, or routing policy.

No local Windows process or network-namespace drawing qualifies the Linux path. A Linux VM is an acceptable initial host if its actual guest kernel meets this contract and snapshot/suspend behavior is controlled. Initial namespace tests use at least two logical CPUs and 4 GiB RAM as the qualification fixture; the physical gateway target has at least 2 GiB RAM. These are selected fixture requirements, not measured minimum hardware requirements for every possible implementation.

The environment manifest records kernel release/config/build ID, module/provider identities, dependency lock/hash, image/distro, architecture, radio/driver, exact interface binding, MTUs, forwarding/firewall/offload state, randomness readiness, clock behavior, allocation accounting, and namespace/cgroup ownership. An unmet requirement rejects qualification of that environment; it does not leave protocol choices open or silently select another mechanism.

## 18. Observability and error semantics

Report administrative state, configuration revision, controller incarnation/lease remaining, peer fingerprint and endpoint, current/retiring/pending handshake identifiers, continuity context, zone, last successful authenticated round trip, local key limits, pair PMTU/RX cap, shared TX MTU, and reason for ineligibility. Never report “HTTP works” from a bound socket or “secure” merely from a completed exchange.

Counters distinguish malformed envelope, wrong endpoint/locator, unknown generation, AEAD failure, replay/too-old/counter-limit drop, invalid plaintext/IP/options/checksum, wrong inner source, missing flow witness, ACL deny, gateway-local/transit deny, MTU/path failure, lease/liveness/key expiry, resource exhaustion, reassembly rejection, and actual inner LAN delivery. Unauthenticated sender attribution remains tentative even if its locator names a configured peer.

Application denials usually time out or receive an appropriate bounded local unreachable/error. There is no promise of one universal browser error or immediate TCP reset. Negative tests keep the same service listener and local-control request healthy, use fresh request identifiers, and correlate server receipt with packet/counter evidence. Captures on the Wi-Fi LANs (the laptops' networks) expose inner plaintext; a capture on the cable exposes VPT1/UDP ciphertext. WPA link encryption on each Wi-Fi network is independent of the VPN.

## 19. Final demonstration contract and conformance evidence

### 19.1 Core physical test method

The physical topology in Section 2 is the proposed final method. All observations use the same Host A process, Host B private IP/HTTP listener, and explicit routes. Hosts have no secondary network, proxy, cloud tunnel, or alternate IPv6 path. Successful local requests from Gateway B keep the service's health independently observable.

| Condition | Expected observation |
|---|---|
| Cable connected, both Wi-Fi networks up; VPN disabled | Outer gateways can use the declared transport path, but A's fresh HTTP request to B fails; local B service check succeeds |
| VPN active with correct identity/policy | Fresh A→B HTTP response succeeds; B sees original source `10.77.1.10`; the cable capture shows the VPT1 carrier and both Wi-Fi LAN captures show the inner path |
| Forbidden port/host/initiation | Denied even while allowed HTTP works; negative destination/listener controls rule out a stopped server |
| VPN disabled again or the cable is unplugged | Protected access fails without direct forwarding between the LANs, NAT, or default-route substitution; counters distinguish administrative stop from path loss |
| VPN re-established after declared failure | Fresh confirmed keys and new application initiation work; pre-failure ciphertext/state does not regain permission |

Unplugging the cable removes the outer path for both gateways. That outcome is expected and cannot be interpreted as a selective peer failure. Stopping only a VPN peer/controller while the cable stays connected is a distinct failure case, as is a Pi's Wi-Fi network going down, which affects only that Pi's laptop.

### 19.2 Multiple hosts and independent peers

With both A hosts and both B hosts present, the three allowed pairs in Section 2 run concurrently; A→B2 remains denied while B2 succeeds for A2 and locally. Disable B2's service and separately remove its permission: B flows remain usable, with evidence distinguishing a host failure from an ACL denial. Return routes, original source addresses, reverse-initiation denial, and no shortcut remain mandatory.

For M, B simultaneously serves A and C with different keys/state. Rekey/restart/revoke A while C remains valid; spoof C's source from A; attempt A↔C transit; and exercise bounded overload. Namespace results are clearly labeled if a third physical gateway is unavailable. Two physical peers plus multiple hosts do not become a three-peer result.

### 19.3 Technical conformance requirements

The overall acceptance cases A01–A43 remain applicable; this technical specification supplies their exact expected mechanisms/limits. The following evidence is additionally necessary before claiming this selected profile works:

| Area | Conformance evidence |
|---|---|
| Crypto primitives | RFC 8439 known-answer vectors; versioned Noise IK known-answer/interoperability vectors; wrong pinned key and malformed DH/input rejection |
| VPT1 exchange | Byte-exact payload/envelope sizes and role bindings; duplicate INIT/RESPONSE; loss of every confirmation phase; timeout before/after commit; rekey grant mismatch; stale configuration completions |
| Key handoff | Correct Split direction at both roles; one installation per attempt; no userspace transport use; duplicate installation cannot reset counters; controlled secret buffers erase on destruction |
| Replay | Counter zero, gaps, reorder distances 1023/1024, window shifts, concurrent duplicates, counter bound, invalid-tag high counter, current/retiring separation |
| State/policy | Midstream TCP cannot create permission; handshake correlations hold; tightened rules beat old conntrack/witness state; selectively removing B2 leaves B flows admitted |
| Kernel ordering | Actual hook/socket/device trace demonstrates the specified order; packet marks cannot create trust; inner normalization precedes conntrack; final admission revalidates current policy |
| Sizes | P=1500/M=1428 and a separately qualified smaller P; DF oversize/DF-clear fragments; no oversized outer output; correct 72-byte expansion; control recovery fits |
| IP/ECN | Two real forwarding hops, exact lengths, options/checksums, all ECN input combinations, permitted ICMP errors including initial-flow and outer-router cases |
| Lifetimes | Soft/hard time, record and byte boundaries; 15-second liveness, five-second lease, retirement deadline, no key or zone rollback |
| Resources | Every packet/byte/object/flow/fragment boundary; shared-host aggregate accounting; starvation and allocation-failure behavior; measured sizes rather than only nominal formulas |
| Teardown | Peer removal, controller loss, namespace-name retention, namespace exit/recreation, device removal, module unload; memory/concurrency diagnostics find no leaks/use-after-free |
| Physical method | Actual Wi-Fi AP and Ethernet cable roles and drivers; no hotspot sharing/NAT; local healthy listener during disabled tests; packet evidence on both Wi-Fi LANs and the cable |

Deterministic keys/nonces are used only in published conformance fixtures. No project-specific ciphertext vector is asserted here without computation. Primitive vectors do not prove the framing/lifecycle composition; passing all applicable evidence and review remains necessary. This is a fully selected design, not an assertion that the absent implementation has passed.

### 19.4 Selected measurement fixture

For the initial service workload, use fresh uncached HTTP requests with a host identifier and request ID, a 1 KiB response for transaction latency, and a 1 MiB response for transfer behavior. Use 50 latency transactions per allowed pair and ten cold concurrent rounds of the multi-host matrix. Record every failure rather than excluding it silently.

Bulk context uses five 30-second TCP trials per allowed data direction, excluding an initial three-second warm-up; UDP diagnostics use 30 seconds at 1, 5, and 10 Mbit/s per peer within the aggregate cap. The diagnostic bulk service is iperf3 on Host B, TCP/UDP port 5201, enabled by explicit temporary source/destination/service rules at both gateways and Host B. It is not implicitly allowed by the HTTP rule or an automatic helper. Reverse TCP data remains within an A-initiated permitted connection; no new B→A connection permission is added. UDP offer tests are A→B only. Removing diagnostic rules restores the original service matrix without invalidating unrelated HTTP flows. Packet-size fixtures cover the configured inner MTU, ±1 around it, DF-clear/DF-set behavior, and small/large UDP payloads. Compare modes in counterbalanced order on the same declared fixture; a directly routed baseline is a separately isolated reachable fixture, not the no-VPN isolation condition.

CPU/memory/packet drops and radio conditions are recorded for the whole gateway/host, including kernel work. Namespace timings and physical Pi results are not pooled. There is no throughput-improvement or numerical latency promise before these observations exist. Deliberately lossy/delayed fixtures test failure semantics rather than supplying clean-path performance averages.

## 20. Resolution of the overall design's decision register

| Overall item | Resolution in this document | What remains evidence rather than a design choice |
|---|---|---|
| D01 secure channel | Sections 3–8: fixed Noise suite/library, Split handoff, AEAD/AAD/nonce, confirmation, replay, and fresh/rekey distinction | Dependency/composition review, interoperability and actual key-handling qualification |
| D02 wire representation | Sections 4–5 and 15: exact fields, sizes, port 51888, endian rules, subtype/length behavior, expansion | Parser/encoding/crypto conformance of actual code |
| D03 platform | Sections 9 and 17: Linux 6.18.y x86-64/ARM64 public API target and mandatory facilities | Exact installed builds, exported/provider availability, Pi radio/AP support |
| D04 operating limits | Sections 7–8, 12, 15–16: finite values, scopes and failure outcomes | Measured allocation sizes/capacity and deadline boundary evidence |
| D05 kernel/policy integration | Sections 9–14: hook order, authoritative state owners, zones/witnesses, normalization, RCU/locks, final admission and teardown | Kernel memory/concurrency/packet-path qualification |
| D06 topology and routes | Section 2 and Section 19: namespace equivalents and Wi-Fi LAN–Pi–Ethernet cable–Pi–Wi-Fi LAN topology | Real addresses/interfaces/listeners/firewalls and no-bypass packet evidence |
| D07 multi-peer | Sections 2.2–2.3, 11 and 19.2: three independent gateways, four-peer cap, no transit | Actual M evidence about physical versus namespace proof |
| D08 measurements | Section 19.4: workloads, repetitions, controls, accounting and comparison conditions | Results and defensible statistical/performance conclusions |

No architecture/protocol/limit alternative in D01/D02/D04/D05 remains implicitly selectable at runtime. Unsupported modes are rejected.

## 21. Sources and assurance boundary

The README and overall design govern the use case/policy. The user's 28 October physical topology clarification supersedes the earlier proposed interface mapping. Primary references support individual mechanisms; they do not certify VPT1's new composition.

- [Noise revision 34](https://noiseprotocol.org/noise.html): fixed suite, handshake/Split semantics, nonce conventions and application responsibilities.
- [snow 0.10.0 handshake source](https://raw.githubusercontent.com/mcginty/snow/v0.10.0/src/handshakestate.rs) and [release record](https://github.com/mcginty/snow/releases/tag/v0.10.0): actual selected exchange component and raw Split interface.
- [Versioned default resolver](https://raw.githubusercontent.com/mcginty/snow/v0.10.0/src/resolvers/default.rs) and [Cargo feature definitions](https://raw.githubusercontent.com/mcginty/snow/v0.10.0/Cargo.toml): primitive/RNG selection and the reason for the explicit DH-result validation boundary.
- [RFC 8439](https://www.rfc-editor.org/rfc/rfc8439.html): AEAD primitive and reference vectors.
- [Linux AEAD API](https://docs.kernel.org/crypto/api-aead.html) and [Linux 6.18 ChaCha20-Poly1305 provider](https://raw.githubusercontent.com/torvalds/linux/v6.18/crypto/chacha20poly1305.c): kernel protection interface and IV/provider distinction.
- [Linux 6.18 UDP tunnel header](https://raw.githubusercontent.com/torvalds/linux/v6.18/include/net/udp_tunnel.h), [Netfilter priorities](https://raw.githubusercontent.com/torvalds/linux/v6.18/include/uapi/linux/netfilter_ipv4.h), and [network-device lifetime documentation](https://docs.kernel.org/networking/netdevices.html): selected kernel integration boundaries.
- [Conntrack controls](https://docs.kernel.org/networking/nf_conntrack-sysctl.html): transport-validation and allocation settings.
- [External module documentation](https://docs.kernel.org/kbuild/modules.html): build/kernel compatibility constraints.
- [RFC 6040](https://www.rfc-editor.org/rfc/rfc6040.html), [RFC 4459](https://www.rfc-editor.org/rfc/rfc4459.html), and [RFC 8085](https://www.rfc-editor.org/rfc/rfc8085.html): ECN, tunnel sizing/reassembly, and UDP-tunnel operating considerations.
- [Linux fragment ECN handling](https://raw.githubusercontent.com/torvalds/linux/v6.18/net/ipv4/inet_fragment.c): concrete ECN merge semantics for the selected normalizer.
- [NetworkManager AP settings](https://www.networkmanager.dev/docs/api/latest/settings-802-11-wireless.html) and [IPv4 method settings](https://www.networkmanager.dev/docs/api/latest/settings-ipv4.html): distinction between AP radio role and shared Internet service.

Selecting standard primitives and a defined packet path resolves design ambiguity; it does not establish a formal security proof or production certification. The technical profile is suitable for the declared educational objective only when the relevant review, functional, adversarial, kernel-lifecycle, and physical evidence supports it.
