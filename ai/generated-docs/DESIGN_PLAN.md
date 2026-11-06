# VPNTunnel: overall system design

**Team:** T005  
**Date:** 17 October 2026  
**Revised:** 31 October 2026<br>
**Version:** 1.5 — Linux 6.18 and inspected Pi 5 deployment target<br>
**Purpose:** Define what the new VPN must do, the boundaries within which it must work, and the evidence needed to substantiate its claims.  
**Status:** Design proposal, not an implementation, a completed qualification, or a production-security certification.

## 1. Purpose, authority, and how to use this document

The intended product is a routed, Layer-3, site-to-site VPN. An ordinary client on one private LAN reaches an explicitly authorized service on another private LAN through independently authenticated gateways. The gateways preserve the private networks as separate routed networks. The final project also aims to process ordinary tunnel data in the Linux kernel and support multiple independent VPN peers.

The defining network path is **Host A → Gateway A → protected tunnel → Gateway B → private HTTP service on Host B**, including the response path. **Initial testing uses Linux network namespaces, not Raspberry Pis:** the client, service host, and each gateway occupy separate namespaces. The proposed final physical arrangement, clarified by the user on 28 October, is Laptop A–Ethernet–Pi A–Wi-Fi–Pi B–Ethernet–Laptop B, with Pi A hosting the AP and Pi B joining it. The Ethernet links are the private LANs and Wi-Fi is the gateway underlay. Encrypted communication between gateway processes alone does not satisfy the objective in either environment.

This document specifies the system's externally meaningful behavior, architectural responsibilities, policy model, security contract, operating envelope, and acceptance evidence. It intentionally contains no coding schedule, source-tree plan, kernel API recipe, shell deployment procedure, or byte-level protocol specification. Those are different documents. Where a later technical specification is required, this document states what it must preserve and what cannot be claimed until it exists.

[TECHNICAL_DESIGN.md](TECHNICAL_DESIGN.md) now supplies the selected VPT1 security protocol, kernel integration, wire format, operating limits, and test-profile details. The overall constraints here remain authoritative for intended behavior; that companion supplies their concrete technical realization. Neither document claims that implementation or qualification has already occurred.

The separate [TEAM_WORK_PLAN.md](TEAM_WORK_PLAN.md), [CODING_STANDARDS.md](CODING_STANDARDS.md), and [TEST_PLAN.md](TEST_PLAN.md) assign the implementation work, coding/review obligations and detailed namespace qualification approach. They do not turn this overall design into an implementation schedule.

### 1.1 Authority and provenance

The authority order is:

1. Explicit team clarifications.
2. The project vision in [README.md](README.md), interpreted consistently with them.
3. This proposed overall design and its explicitly identified team decisions.
4. Previous project material, used only for historical evidence and lessons.

Explicit team decisions supplied with this design refine the README's open choices. In particular, the user's namespace-testing clarification governs the initial test environment. It does not change the underlying connectivity objective. Choices made by this document remain proposals unless explicitly confirmed by the team; calling one a baseline does not mean it is approved.

In this document, **must** identifies a condition of the proposed design; **should** identifies a justified preference that needs a recorded reason if changed; **may** identifies an optional capability. These words do not turn team decisions into external requirements.

### 1.2 What “valid” means here

A defensible design has consistent requirements, explicit assumptions, defined failure behavior, and falsifiable acceptance criteria. It cannot guarantee that every future implementation, hardware configuration, or network will be free of defects. The goal is to eliminate hidden contradictions and unsupported claims, then require evidence against the remaining risks.

Three statuses must remain separate:

- **Specified:** this document defines intended behavior.
- **Qualified:** that behavior has passed the relevant evidence requirements on a recorded environment.
- **Approved:** the relevant project owner has accepted a decision or scope.

Nothing in this document implies that qualification or approval has already happened.

### 1.3 Reading guide

| Question | Sections |
|---|---|
| What are we building, and how is it different? | 2–4 |
| What connects to what, and how do packets return? | 5–8 |
| Who can access which services? | 9–10 |
| What protects traffic, and what happens during change or failure? | 11–15 |
| What can we claim about kernel processing and performance? | 16–19 |
| What evidence constitutes success? | 20–23 |
| What still requires external confirmation or a technical specification? | 24–26 |

## 2. What changes from the earlier project

The historical project lives in `../CS331-T005-MinimalVPNTunnel/`. Its code README, architecture, protocol documentation, source, and report describe a userspace TUN/UDP tunnel between two Pi endpoints. That work is useful evidence of learning, but its addressing and measurements do not establish the new site-to-site use case.

| Dimension | Earlier implemented project | New design |
|---|---|---|
| Principal application endpoints | The two tunnel hosts | Ordinary LAN hosts behind the tunnel gateways |
| Principal connectivity evidence | Traffic to the other Pi's tunnel address | Access to the same otherwise inaccessible private service on another LAN |
| Packet-processing location | Userspace TUN read/write and UDP loop | Kernel data path is a team requirement for the final kernel profile |
| Network relationships | One configured endpoint pair | Independent peer relationships with distinct permissions and state |
| Authorization | Pinned remote endpoint identity and packet checks | Identity, source-network ownership, destination/service policy, initiation direction, and return-state checks |
| Session lifecycle | Reported absence of session rotation and final handshake confirmation | Confirmed establishment, bounded rekey overlap, hard expiry, restart recovery, and local revocation at the committed authorization boundary |
| Addressing | A private /30 connecting tunnel endpoints | Distinct non-overlapping LAN prefixes; no extra endpoint /30 required by the overall design |
| Packet sizing | Fixed inner limit in the historical code | A declared, verified path budget with explicit oversize and path-change behavior |
| Demonstration claim | Educational protected host-to-host traffic | Controlled private-resource access across separate networks |
| Quality claim | Educational prototype | Educational system qualified only within its documented operating envelope |

The old implemented data header is documented as 28 bytes. The separate historical `docs/NEW_VPN_DESIGN.md` proposed a different 14-byte format and broader production-oriented scope. **Neither is adopted here.** The latter is a proposal, not evidence that those features were implemented.

We retain the useful separation of inner and outer traffic, packet captures, explicit peer trust, and measurement discipline. We do not copy old code, adopt its handshake, assume its throughput will transfer to a new topology, or assume that header size explains its performance. Historical sources are listed in Section 26.

## 3. Requirements and scope

### 3.1 Requirements with traceable intent

| ID | Required outcome | Origin within the available material |
|---|---|---|
| R01 | Demonstrate private-service access across distinct LANs | README's project vision |
| R02 | Use an ordinary application and explain request and response paths | README core objective |
| R03 | Establish no unintended baseline path to the private service | README demonstration requirement |
| R04 | Route selected private destinations through a Layer-3 split tunnel | README working direction; adopted design choice |
| R05 | Permit explicit services and initiation directions, with default denial elsewhere | README policy questions; concretized here |
| R06 | Authenticate peers and protect carried traffic against reading, modification, and replay | README final security goal |
| R07 | Process normal tunnel data in the Linux kernel | Team goal stated in README |
| R08 | Support multiple independently identified and configured peers | Team requirement stated in README |
| R09 | Fail closed for protected destinations on loss of usable protection | README security/failure intent; concretized here |
| R10 | Define packet sizing, bounded resources, session lifecycle, and safe configuration changes | Additional quality requirements proposed here |
| R11 | Support claims with packet-path evidence and fair measurements | README success and performance criteria |
| R12 | Distinguish intended features, demonstrated behavior, and unresolved decisions | README authority and evidence principles |

### 3.2 Separate qualification profiles

These are groups of claims, not a delivery schedule. Completing one does not automatically complete the others.

| Profile | What it establishes | Minimum participants |
|---|---|---|
| **C: Connectivity and policy** | Authorized private HTTP access, return routing, isolation, split routing, and failure behavior | Two gateways, two separate LANs, client and service hosts |
| **S: Protected operation** | C plus mutually authenticated transport, integrity, confidentiality, replay rejection, and safe session lifecycle | Same topology, plus adversarial traffic and identity tests |
| **K: Kernel data path** | S with evidence that routine data forwarding and cryptographic packet processing stay in the kernel | Qualified Linux gateway environment |
| **M: Independent multi-peer operation** | S plus concurrent independent peers, distinct permissions, revocation, and isolation | At least three independently keyed gateways |

The project's intended final result satisfies **C + S + K + M**. C captures the primary networking objective; K and M must not displace it. An unencrypted explanatory experiment may illustrate C in an isolated lab, but is not the operational VPN and cannot satisfy S. Such an experiment must not be an automatic fallback mode of the protected service.

The additional lifecycle, adversarial, and platform criteria below qualify the relevant team claims. They are not prerequisites for the first connectivity demonstration. Network/path and policy rules apply to C; cryptographic session/key rules apply to S and its dependent profiles. An explicitly labeled unencrypted C experiment uses its own isolated configuration and cannot claim the security-specific invariants.

Evidence for several hosts on both LANs uses the two-gateway extension in Sections 5.5 and 9.4 and acceptance case A28. It is distinct from M: two clients and two service hosts still use only the A–B peer relationship. The basic one-client/one-service demonstration remains the minimum C scenario.

### 3.3 Supported baseline

- Linux gateways; ordinary client and server applications need no VPN software. Initial functional testing uses separate Linux network namespaces on a qualified Linux kernel; Pi hardware is not a prerequisite for these tests.
- IPv4 inner packets over IPv4/UDP between explicitly configured gateway endpoints.
- Non-overlapping site prefixes and a separate underlay prefix.
- Static gateway endpoints on an administered lab underlay with known path properties.
- Routed unicast connectivity; no shared Ethernet segment.
- Explicit service permissions. HTTP over TCP is the primary demonstration; controlled diagnostic ICMP and separately permitted UDP can demonstrate the Layer-3 abstraction.
- One direct tunnel relationship for each permitted site pair. Multi-peer demonstration uses A–B and C–B relationships with B hosting the shared service site.
- No source or destination address translation for protected inner traffic.
- Per-peer state, bounded queues, administrator-provisioned trust, and kernel packet processing for profile K.

### 3.4 Exclusions

The baseline does not include IPv6 carriage, an Ethernet-bridging VPN, multicast/broadcast extension, automatic LAN discovery, overlapping subnet translation, full-tunnel Internet egress, automatic route advertisements, peer-to-peer transit through another peer, automatic NAT traversal, relays, endpoint roaming, multipath, high availability, human accounts, or commercial/production certification. Local testbed bridges used to construct a LAN or the underlay are permitted; Ethernet frames are not carried between site LANs by the VPN.

It also does not promise arbitrary IP-protocol support merely because it operates at Layer 3. TCP, authorized UDP, and necessary ICMP are the qualification scope. Other inner protocols are denied unless added through an explicit design revision.

Internet deployment is not implied by choosing UDP. It needs a separate qualification of endpoint reachability, NAT behavior, path MTU, congestion behavior, and operational exposure. Excluded features may be proposed later, but cannot appear as silent fallbacks.

The NAT exclusion needs precise attribution. The README excludes a promise of automatic connectivity through **every** NAT arrangement; it does not say that NAT traversal is prohibited or that no bounded NAT scenario may ever be supported. 

Inner address translation and outer NAT traversal are separate decisions. Inner SNAT would change the client address seen by the remote service, which this design avoids. A NAT on the outer path changes the gateway carrier's endpoint mapping; accommodating it need not translate inner client addresses. Establishing and maintaining reachability across that outer mapping requires a declared arrangement, such as an administered port mapping or a separately designed traversal mechanism. UDP encapsulation alone does not provide it. The current static endpoint rules have not qualified either arrangement and would need explicit revision before a NAT-based deployment is claimed. [RFC 5128 — Communication across NATs](https://www.rfc-editor.org/rfc/rfc5128.html)

## 4. Principal architectural decisions

| Decision | Selected behavior | Reason and consequence |
|---|---|---|
| VPN model | Gateway-to-gateway, site-to-site | LAN applications remain ordinary applications |
| Network abstraction | Routed Layer 3 | Separate subnets and explicit routing; ARP remains local |
| Traffic selection | Split tunnel with explicit remote-prefix ownership | No default-route takeover or implicit Internet exit |
| Outer transport | UDP over IPv4 | Datagram carriage without a second reliable byte stream; loss remains visible to inner protocols |
| Initial testbed | Separate host and gateway Linux network namespaces, connected by veth links and an isolated virtual underlay | Exercises routed site boundaries without requiring Pi hardware |
| Later physical topology | Each Pi hosts a Wi-Fi network for its local laptop and the two Pis are joined by an Ethernet cable (decision 5 November 2026; replaces the 28 October Ethernet-LAN/Wi-Fi-underlay proposal) | Matches the README's example, preserves separate private LANs with one radio role per Pi and needs no NAT |
| Normal data processing | Kernel virtual network interface and kernel encapsulation/protection | Meets the team's data-path goal; superiority must be measured |
| Administration | Local privileged control service/tool | No remote management portal or account platform required |
| Key exchange responsibility | Established mutually authenticated key-exchange mechanism through a separate control component | Avoid a new cryptographic protocol; exact secure-channel profile is a gated technical decision |
| Identity | Pinned, independently generated public-key identity per gateway | No group key, trust-on-first-use, or address-as-identity |
| Network authorization | Static administrator-owned prefix mappings and explicit service rules | Authentication does not authorize route announcements |
| Failure policy | Deny protected cross-site traffic whenever prerequisites fail | No plaintext, default-route, or alternate-peer fallback |
| Address translation | None for inner traffic | Original client address remains visible and the return path is explicit |
| Multi-peer shape | Independent pairwise relationships; no spoke-to-spoke transit | Proves peer independence without adding routing through intermediate sites |

Using an established cryptographic component does not mean copying the previous VPN or claiming protocol compatibility with WireGuard. The team's contribution is the new tunnel system, its gateway data path, routing/policy integration, lifecycle, observability, and demonstrated behavior.

## 5. Initial namespace testbed and later physical topology

### 5.1 Initial two-site arrangement: Linux network namespaces

The initial testbed has five network namespaces: `host-a`, `gw-a`, `underlay`, `gw-b`, and `host-b`. Each host namespace runs an ordinary application. Each gateway namespace has a LAN interface, an outer interface, its own VPN configuration and identity, and independent session state. The `underlay` namespace contains an isolated Layer-2 bridge joining only gateway outer links. Its bridge and switch-side ports have no IP address or Layer-3 forwarding role in the baseline.

```text
                  One Linux kernel; separate network namespaces

 host-a                  gw-a                    gw-b                 host-b
 10.77.1.10        LAN 10.77.1.1             LAN 10.77.2.1           10.77.2.10
 HTTP client ---- veth LAN A                 veth LAN B -------- HTTP :8080
                          |                       |
                 outer 172.31.250.11     outer 172.31.250.12
                          |                       |
                          +-- veth --+-- veth -----+
                                     |
                              underlay namespace
                           isolated outer-only bridge

             gw-a <======= protected UDP relationship =======> gw-b
```

Each LAN is a distinct virtual Ethernet segment using a host↔gateway veth pair. The gateway routes between LAN and tunnel; it never bridges LAN to underlay. The underlay bridge supplies the transport network and does not join the private LANs. This preserves the same routed request/response path that will later use physical interfaces. Network namespaces isolate devices, routes, firewall rules, and sockets; veth pairs supply the links between them. [Linux network namespaces](https://man7.org/linux/man-pages/man7/network_namespaces.7.html), [veth devices](https://man7.org/linux/man-pages/man4/veth.4.html)

The root/initial network namespace is an orchestration context, not an extra data-plane router. The test topology has no attachment to the host's ordinary network, no root-namespace NAT, no host-network service proxy, and no direct host-a↔host-b link. Host applications and tunnel sockets must actually run in their assigned namespaces; executing every application in the root namespace would invalidate the intended topology.

The shared host has no application IP attachment to the isolated underlay bridge. Administrative namespace access is local orchestration, not a network path from the client to the service. Capturing packets is passive unless a separately declared fault-injection experiment explicitly sends them.

Use the address inventory below with static addresses initially. Host A and Host B have only their respective LAN attachment and loopback. Their cross-site routes use their own gateway. An additional host behind A uses another host namespace on an A-only LAN bridge; it is not connected to the underlay. For M, add `gw-c` and `host-c`, connecting only C's outer interface to the same isolated underlay bridge.

Peer identity, prefix ownership, connection state, replay state, counters, and control-plane actions must be scoped to the correct gateway namespace. Reusing a UDP port number across separate gateway namespaces must not merge sockets or sessions. Namespace deletion/recreation cannot recover old sessions or leave cross-site routes in another namespace. Distinct key material must be provisioned explicitly: network namespaces alone do not isolate files or automatically generate separate identities.

All these namespaces share one kernel and physical machine. They establish logical network separation, not independent operating-system compromise boundaries or separate hardware failure domains. Namespace tests can substantiate C, S, M, and—if the actual data path meets Section 16—K on that recorded Linux environment. They do not establish Pi compatibility, Wi-Fi/AP operation, independent-machine clock behavior, or physical-network performance.

### 5.2 Later physical demonstration mapping

| Logical role | Initial testbed | Later physical arrangement |
|---|---|---|
| Host A / Host B | `host-a` / `host-b` namespaces | Ordinary client / service laptops |
| Gateway A / Gateway B | `gw-a` / `gw-b` namespaces | Pi A / Pi B |
| Site A / Site B LAN | Separate veth LAN segments | Separate Pi↔laptop Ethernet segments; a local switch supports additional physical hosts |
| Underlay | Outer-only bridge in `underlay` namespace | The Ethernet cable between the two Pis |
| Independent peer C | `gw-c` plus `host-c` | Additional independently configured gateway and host, if required for physical evidence |

The logical addresses, ownership, routes, permissions, and failure expectations remain the same when moving to physical equipment. The shared underlay connects **gateway outer interfaces only** and does not bridge the LANs. An observation point may capture outer traffic but must not create an alternative forwarding path.

For the later setup, each Pi's laptop-facing Wi-Fi interface and its outer Ethernet interface have distinct IP roles. Each Pi hosts its own WPA2 Wi-Fi network for one laptop, and neither Pi bridges Wi-Fi into the cable. Each laptop uses only its own Pi's Wi-Fi network; the cable, the other Pi's network, tethering, proxy and other VPN paths must be disabled or explicitly ruled out with evidence. AP-capable radio/driver support, Ethernet availability, power and the selected Pi image require their own qualification (AP mode ran on one Pi 5 on 4 November 2026). AP Internet-sharing/NAT defaults are not permitted. This arrangement is the README's own example of each Pi advertising a Wi-Fi network for its local laptop; the 28 October Ethernet-LAN proposal it replaces is withdrawn. These hardware checks do not block namespace testing.

### 5.3 Address inventory

Throughout the remaining design, Host A/B/C and Gateway A/B/C denote logical roles. They map to namespaces initially and physical devices later; routing and policy rules apply to both.

| Entity | Address or prefix | Function |
|---|---|---|
| Site A | `10.77.1.0/24` | Local host network |
| Gateway A LAN | `10.77.1.1` | Next hop for A's remote-service traffic |
| Host A | `10.77.1.10` | Initially authorized client |
| Host A2, multi-host extension | `10.77.1.11` | Second client behind the same Gateway A |
| Site B | `10.77.2.0/24` | Private service network |
| Gateway B LAN | `10.77.2.1` | Return next hop from Host B |
| Host B | `10.77.2.10` | Service at `http://10.77.2.10:8080/` |
| Host B2, multi-host extension | `10.77.2.11` | Separate service host at `http://10.77.2.11:8080/` |
| Underlay | `172.31.250.0/24` | Gateway transport only |
| Gateway A outer | `172.31.250.11` | A's configured transport endpoint |
| Gateway B outer | `172.31.250.12` | B's configured transport endpoint |
| Site C, for M | `10.77.3.0/24` | Independent additional client site |
| Gateway C LAN / outer | `10.77.3.1` / `172.31.250.13` | C's local next hop and transport endpoint |
| Host C | `10.77.3.10` | Separately authorized client for M |

These are concrete proposed lab allocations, not discovered device addresses. The administrator must check them against connected networks, other VPNs, and existing routes before adopting them. If a collision exists, replace the allocation consistently across the topology, peer ownership, routes, and policy. Overlap must be rejected, not resolved by an undocumented priority rule.

An additional numbered tunnel subnet is not needed to explain forwarding between LAN prefixes. A virtual interface may be unnumbered for this purpose. Any later management addresses on the tunnel need their own policy and must not replace the laptop service as the main demonstration.

### 5.4 Local-network and service responsibilities

- Each LAN has exactly one intended address-management authority. Static addresses or DHCP reservations are acceptable; duplicate addresses are not.
- DHCP and ARP remain within the local LAN. The tunnel does not forward DHCP broadcasts or make remote ARP work.
- Host B listens on its private address and port 8080. Its host firewall permits the authorized A client and local service verification. Under profile M, C is added explicitly.
- Host B's return route to A and C uses Gateway B. A competing default gateway must not receive that return traffic.
- The private service is not published on an underlay or public address by port forwarding, a reverse proxy, a cloud tunnel, or another VPN.
- LAN access is itself administered. Wi-Fi credentials do not authenticate an individual laptop to the remote VPN gateway.

The baseline requires no Internet access. If ordinary Internet access is added, it is a separately documented local service: cross-site destinations still cannot escape through its default route or NAT. A split-tunnel claim means unrelated routes remain unchanged, not that Internet access magically exists.

For local service verification in the minimal namespace topology, an ordinary client in `gw-b` connects to Host B over the B LAN, using `10.77.2.1` as its source; Host B's local policy allows that check. This is a local test only and creates no gateway-originated cross-site service permission. It proves a real LAN request can reach the listener rather than merely testing a loopback response. The same B service binding and local check remain available while A's cross-site access is tested with the VPN disabled.

### 5.5 Multiple clients and multiple service hosts

The multi-host extension retains `gw-a`, `gw-b`, and their single peer relationship. It adds `host-a2` for Host A2 and `host-b2` for Host B2, each as a distinct host namespace. Existing Host A and Host B remain the original client and service host. Each service binds its own private address and port 8080 and returns a distinguishable host/service identifier plus the fresh request identifier.

Each site needs a genuine local segment for its three attachments: two hosts and the gateway's LAN interface. In this extension, `lan-a` and `lan-b` are switch-only namespaces with unnumbered LAN bridges. Their veth ports connect only their own site's host namespaces and gateway LAN interface. These local bridges replace the minimal point-to-point LAN links; neither joins the underlay bridge or the other site. No forwarding, NAT, or service proxy in the root namespace is introduced.

```text
host-a  ---+                                      +--- host-b  HTTP :8080
           |                                      |
         lan-a --- gw-a ==== protected ==== gw-b --- lan-b
           |                                      |
host-a2 ---+                                      +--- host-b2 HTTP :8080
```

Host A2 routes `10.77.2.0/24` through `10.77.1.1`; Host B2 routes `10.77.1.0/24` through `10.77.2.1`. The original hosts retain their existing routes. The gateways retain their existing prefix ownership; adding hosts neither adds a peer nor expands a source/service rule automatically. If C is present, B2 still has no C permission by default, and any return route to C is installed only for an explicitly allowed C–B2 service case.

Both B hosts must pass the local LAN service check from `gw-b` independently of the tunnel. Their host firewalls mirror the explicit cross-site permissions in Section 9.4 while permitting that local verification. Evidence for the extension covers concurrent allowed flows, the denied source/destination combination, original source-address visibility, and independent service-host failure. A second process on Host B or an additional IP alias alone is not evidence of a second LAN host.

## 6. Routing and address ownership

### 6.1 Intended routes

| Device | Destination | Intended next hop / handling |
|---|---|---|
| Host A | `10.77.1.0/24` | Local LAN |
| Host A | `10.77.2.0/24` | Gateway A, `10.77.1.1` |
| Gateway A | `10.77.1.0/24` | Local LAN interface |
| Gateway A | `10.77.2.0/24` | VPN interface, uniquely owned by peer B |
| Gateway A | `172.31.250.12` | Underlay interface, never the VPN |
| Gateway B | `10.77.2.0/24` | Local LAN interface |
| Gateway B | `10.77.1.0/24` | VPN interface, uniquely owned by peer A |
| Gateway B | `172.31.250.11` | Underlay interface, never the VPN |
| Host B | `10.77.1.0/24` | Gateway B, `10.77.2.1` |
| All devices | Unrelated destinations | Existing explicitly configured route, or unreachable if none exists |

Host routes may be specific routes or follow a local default through the gateway. The observable requirement is the same: the remote destination and its return traffic reach the correct gateway. The VPN does not rewrite the laptop's application destination.

The initial namespace baseline chooses explicit remote-prefix routes on the hosts, with no default route. The local LAN and remote-prefix routes above are therefore the complete necessary client/service routing. Any later default route is a declared profile change and must preserve the private-prefix guards. The gateways have connected underlay routes for their peer endpoints; the table lists the important endpoint selections without requiring separate host routes where the connected route already suffices.

For M, Gateway B additionally owns a route to `10.77.3.0/24` through peer C, Host B returns C traffic through Gateway B, and Gateway C routes `10.77.2.0/24` through B. A and C receive no route or permission for each other's site through B.

### 6.2 Routing and authorization are separate

A route to `10.77.2.0/24` identifies where traffic would go. It does not permit all hosts or ports in that prefix. The initial service permission is only the flow class in Section 9. Routing a larger prefix makes unauthorized destinations predictably denied rather than accidentally sent through some unrelated default path.

Each gateway has an administrator-owned **destination-prefix → peer** map and a separate **peer → permitted inner source prefixes** map. In the baseline, a remote site's prefix has exactly one owner. Duplicate, overlapping, default, multicast, loopback, link-local, local-LAN, and underlay prefix assignments are rejected. A peer cannot grant itself a prefix during a handshake.

Rejection of local-LAN and underlay prefixes applies to assigning those prefixes to a **remote peer**; it does not prohibit the gateway's own connected networks. A valid source address must also be a unicast host address in the assigned range, not that prefix's subnet/broadcast address. On the local LAN, addresses belonging to the gateway itself cannot be accepted as another host's source.

An authenticated packet arriving from A with an inner source from C is rejected even if both peers may reach the same HTTP server. Source ownership is checked before a packet can create allowed connection state.

### 6.3 Underlay routing must remain independent

Outer peer addresses use an explicitly understood underlay route. Tunnel destination routes must never capture the tunnel's own UDP traffic. Interface, source-address, or default-route changes that make that guarantee uncertain suspend the affected relationship until it is revalidated.

In the baseline, no transparent endpoint migration occurs. A new outer address is a configuration change and requires a fresh authenticated relationship. A packet from an unexpected address cannot update endpoint state merely because it names an existing session.

An endpoint is the configured outer IP address and UDP port pair, not just an address. The service binds to its gateway's underlay attachment in the correct namespace, uses the configured source endpoint, and receives only explicitly configured peer endpoint traffic for further protocol validation. Control and data use the declared VPN service endpoint; this does not expose a management API. The technical companion selects UDP port 51888. This static directly reachable profile excludes even accidental dependence on a NAT mapping or roaming source port. Source-endpoint filtering is an exposure restriction, never a substitute for identity authentication.

### 6.4 Persistent protection of private destinations

Protected prefixes remain classified as protected when the virtual interface, daemon, or session disappears. Removing an active VPN route must not reveal a usable default route to the same prefix.

This is a system-wide invariant supported by both routing and firewall policy: private cross-site traffic is either carried by its authorized protected tunnel or denied. Deny rules must also cover policy routing, locally generated traffic where applicable, and underlay egress. A route deletion by itself is not a sufficient failure policy.

Protection is defined over the declared remote prefixes, including unreachable reserved destinations, rather than only a list of currently established flows. Disabling or revoking a peer does not automatically withdraw those guards. Updating or removing a guard requires an explicit administrative decision about the resulting exposure.

## 7. Components and trust boundaries

```text
Trusted local administration
       |
       v
Configuration / lifecycle control -------> Established key-exchange component
       |                                   (control traffic only)
       | validated policy and session state          |
       +-----------------------+---------------------+
                               v
                        Linux gateway kernel
LAN IP forwarding -> service policy -> VPN peer selection -> protected UDP output
LAN IP forwarding <- service policy <- authenticated inner IP <- UDP receive
                               |
                        Untrusted underlay
```

| Component | Owns | Must not implicitly own |
|---|---|---|
| Ordinary LAN host | Application and local IP stack | Peer keys, tunnel framing, remote peer discovery |
| Gateway routing | Local next hops and remote-prefix selection | Permission to use every routed resource |
| Stateful policy | Initiation rules and permissible replies | Peer identity verification |
| VPN data plane | Encapsulation, packet protection, replay acceptance, peer source checks | Trusting a remote route announcement |
| Key-exchange component | Peer authentication and fresh session establishment | Granting new LAN/service permissions |
| Local control plane | Validated configuration and lifecycle coordination | Forwarding every application packet |
| Administrator | Key trust, site ownership, policy, declared operating profile | A guarantee that authenticated hosts are uncompromised |
| Observation layer | Counters, state, diagnostics, evidence | Logging secrets or plaintext by default |

The proposed separation permits userspace key-exchange and management while kernel processing handles normal data. This satisfies the README's kernel data-path objective; it does not claim that every handshake operation runs in the kernel. Loss of the supervising lifecycle controller closes the operational service as specified in Section 13. An isolated key-exchange failure follows the negotiation/rekey rules and does not itself revoke a still-valid current session.

Both gateways are trust endpoints for carried plaintext. Protection covers the underlay between them. Plain HTTP is visible on the LAN sides and to the gateways; application TLS can provide a separate end-to-end property if desired.

## 8. End-to-end packet behavior

### 8.1 Allowed request

For a new connection from `10.77.1.10:ephemeral` to `10.77.2.10:8080`:

1. Host A generates an ordinary TCP/IP packet. Its route points to Gateway A. Local ARP resolves Gateway A's LAN address, not Host B's address.
2. Gateway A receives the packet from its LAN, checks source validity and the service/connection policy, and selects peer B for the destination prefix.
3. Gateway A verifies that B has a usable confirmed session, a valid path budget, and current permission. Without these, it denies transmission instead of falling back.
4. The kernel encapsulates and protects the inner IP packet. The outer IPv4 source/destination are the gateway outer addresses; the UDP port is a separately configured transport parameter, not port 8080.
5. The underlay carries the outer packet to Gateway B. It does not route directly to the laptop's private destination.
6. Gateway B validates the record, authenticates it, applies replay protection, verifies that peer A owns the inner source, and enforces the current service/connection policy.
7. Gateway B routes the accepted inner packet onto Site B's LAN. Normal local neighbor resolution finds Host B.
8. Host B's host firewall and HTTP listener accept the request. The application sees the original client address `10.77.1.10`.

The two gateways perform ordinary router TTL processing and checksum handling. Encapsulation must not introduce extra hidden router hops or skip the forwarding hops. The outer IP TTL is independent of the inner TTL. The chosen packet-path integration must demonstrate correct behavior rather than applying an extra manual TTL decrement indiscriminately. IPv4 forwarding and ICMP semantics are grounded in [RFC 1812](https://www.rfc-editor.org/rfc/rfc1812.html).

### 8.2 Return traffic

Host B generates a response from `10.77.2.10:8080` to the client's original address and port. Its route sends the response to Gateway B. Gateway B permits it only as the reverse traffic of the authorized connection, selects peer A, and protects it. Gateway A authenticates B, checks B's inner source ownership and return-state policy, then forwards to Host A.

There is no inner SNAT. A server-side route error is therefore visible as a return-path failure and must be fixed as such. It must not be concealed by adding translation without revising the design.

### 8.3 Non-allowed paths

- A new B-initiated connection into A is denied even if it uses TCP source port 8080.
- An A request to another B host or unapproved B port is denied.
- A packet for B received directly from the underlay as plain inner IP is denied.
- A packet from a valid peer claiming another site's source is denied.
- Traffic unrelated to configured protected destinations follows its existing route, subject to local policy.

## 9. Service policy and connection initiation

### 9.1 Initial allow matrix

All cross-site permissions are default-deny. Gateway management and local-LAN access are distinct policies.

| Source / direction | Destination / traffic | Initial decision |
|---|---|---|
| Host A `10.77.1.10` | Host B `10.77.2.10`, new TCP connection to port 8080 | Allow |
| Host B | Reverse packets belonging to that allowed connection | Allow while state and current policy remain valid |
| Site B | New independent connection to Site A | Deny |
| Any other Site A host | B HTTP service | Deny until explicitly added |
| Host A | Other B host, other B TCP port, or arbitrary UDP | Deny |
| Either LAN | Remote gateway's SSH, control API, or management services | Deny |
| Underlay host | Either private LAN directly | Deny |
| Underlay peer endpoint | Configured VPN control/data UDP service | Admit only to protocol validation; not automatic authentication |
| Underlay router | Valid ICMP error quoting this gateway's recent outer traffic | Permit bounded path-error validation; do not require the router's source to equal a VPN endpoint |
| Unconfigured endpoint/identity | Tunnel establishment or data | Deny |
| Valid flow-related ICMP errors | Affected authorized flow | Permit narrowly as described below |
| Diagnostic echo requests | None in normal service profile | Require a temporary explicit diagnostic rule |
| A ↔ C through B under profile M | Any new transit flow | Deny |

For profile M, `10.77.3.10 → 10.77.2.10:8080/TCP` is a second explicit allow rule with separate state. The multi-host extension uses the distinct permissions in Section 9.4. These are additional host/service rules behind the existing gateways, not new VPN identities; they do not replace the initial allow matrix unless the extension's configuration is explicitly selected.

### 9.2 Stateful permission semantics

A new TCP flow is allowed only when its source, destination, protocol, destination port, ingress identity, and initiation direction match a current allow rule. Only valid connection state admits later packets and their reverse direction. Unsolicited ACKs, a matching source port, or an existing unrelated connection do not create permission.

An allowed TCP flow starts with a valid initiating SYN in the permitted direction and progresses through the expected handshake. The policy does not adopt an already established stream from observed midstream packets after state loss. Merely accepting an operating-system connection-tracking label is insufficient if its settings allow that behavior. The Linux connection-tracking controls explicitly distinguish picking up established connections; the relevant implementation must preserve this design's initiation rule. [Linux connection-tracking documentation](https://kernel.org/doc/html/latest/networking/nf_conntrack-sysctl.html)

State is scoped to the peer relationship and the complete inner flow identity. Stale state must not authorize a newly assigned peer or reused address. A policy tightening or peer revocation removes affected permission even for previously established flows; an unconditional “allow established” rule must not override revocation.

Connection permission is bound to the local policy revision and the relationship's continuity context, not only the IP/port tuple. Successful ordinary rekey preserves that context. Fresh establishment, idle expiry, or an explicit disruptive change cannot recover it from packet headers. The baseline does not synchronize firewall flow tables between gateways: each gateway observes and validates the allowed initiation and its responses on its own path. Loss of either gateway's state can therefore require a fresh application connection even when the other still remembers the old flow. Dropping permission does not guarantee an immediate TCP reset or application notification.

If UDP services are later enabled within the baseline's protocol scope, each rule needs a destination port, allowed initiator, bounded reply association, and idle expiry. The association is not proof of application authentication. Generic automatic helper protocols that open extra ports are disabled unless explicitly designed and qualified.

ICMP errors required for permitted traffic are not treated as new application sessions. Supported classes are applicable destination-unreachable errors, including fragmentation-needed, and time-exceeded errors. They need valid structure and a sufficiently matching quotation of a recently admitted packet/path, and are rate-limited. No rule accepts arbitrary ICMP merely because a quoted destination is inside an allowed prefix. Redirects, source-quench messages, and unsupported control types do not update routes, trust, or permissions. Echo requests/replies need their own diagnostic association and rule.

Errors may quote an authorized first packet even before a normal reply has established a full flow, so admission cannot require an already completed TCP connection. A gateway-generated error uses that gateway's local LAN address as its inner source. When tunneled, that source falls within the gateway's site ownership and must pass the receiving peer source check. The error itself is separately authorized by its valid quotation; it does not need to match a new HTTP connection rule. This permits Gateway B to report an error to A or C without permitting B-initiated application connections.

Outer-path ICMP is separate. A router's outer error is delivered to the local gateway's path-validation logic and checked against recent outer transmissions, including plausible type/code, quotation, endpoint/port, and MTU information. It is not authenticated peer data and cannot directly become an arbitrary inner packet or reconfigure trust. A usable inner error is generated locally only when a validated outer event can be associated safely with the affected inner traffic. Flow association never waives source ownership for tunneled inner packets.

### 9.3 Distributed enforcement

Gateway A rejects unauthorized traffic near its source; Gateway B is the final gateway enforcement point for B's resources. The host firewall on Host B provides a further local boundary. The effective permission is the intersection of both gateway policies and the service host policy.

Each site administrator controls its own exports. A peer cannot broaden the remote site's policy by proposing a different configuration. Inconsistent policies may cause a visible denial; they must not cause broader access. Policy revisions and counters must make mismatches diagnosable.

The receiving gateway permits service forwarding only onto its own declared service LAN. It denies tunnel-to-tunnel transit, forwarding onward through the underlay, and delivery to gateway-local services except the explicitly authorized router-error/diagnostic handling. Gateway-generated cross-site application connections are also denied in the initial policy. These checks cover locally addressed packets as well as forwarded packets; controlling only the forwarding path would leave the gateway's own network services outside the intended boundary.

### 9.4 Access matrix for the multi-host extension

This extension explicitly adds two rules to the original Host A→Host B rule: Host A2→Host B and Host A2→Host B2. All apply only to TCP destination port 8080. The differing destination permissions demonstrate that adding a host does not grant access to every exported service host.

| Client initiating a new connection | Host B `10.77.2.10:8080` | Host B2 `10.77.2.11:8080` |
|---|---|---|
| Host A `10.77.1.10` | Allow | Deny |
| Host A2 `10.77.1.11` | Allow | Allow |

Replies are permitted only for the corresponding allowed flow. Neither B host may initiate new connections into either A host. Other ports and unlisted source/destination combinations remain denied. If profile M runs concurrently, Host C retains only its separately configured access to Host B; Host B2 is not implicitly exported to C.

The gateways and each service host enforce this matrix. A28 exercises the three allowed combinations concurrently, verifies that Host A→Host B2 fails while B2's listener works for Host A2 and its local check, and tests reverse initiation independently. Removing B2's permission or stopping its listener must not revoke A–B peer trust or disturb still-authorized flows to Host B. A later policy may permit all four combinations, but that is a separate explicit policy revision and needs corresponding evidence.

## 10. Identity, host trust, and peer ownership

A **peer** is a gateway authenticated with its own provisioned public key. A **host** is a device using a LAN behind a peer. A **session** is a temporary protected association between two peers. A **flow** is an inner application conversation. These four concepts must never share one undifferentiated identity or lifetime.

Every peer has independent trust metadata, authorized prefixes, endpoint configuration, sessions, sequence/replay state, resource accounting, and counters. A peer identifier used for lookup is not itself a cryptographic identity. A session locator received from the network is only a lookup hint until authentication succeeds.

A gateway has its own identity, which may legitimately be pinned by several other gateways. Thus A and C both pin B's gateway identity; B does not need a different long-term identity for each relationship. What must be independent are the A–B and C–B session keys and state. Duplicate peer entries for one identity within a single local gateway configuration are rejected, while the same remote identity in different gateway configurations is normal. Reusing one identity for distinct gateway instances A and C is prohibited by the testbed's identity policy.

Public keys are verified through an authenticated administrative channel or a directly checked fingerprint. There is no automatic trust on first contact, shared group key, password-based sign-in, or key enrollment based only on LAN membership.

The gateway attests to a source **site/prefix**, not necessarily to an individual person or physical laptop. An address allowlist such as `10.77.1.10/32` depends on Site A's local anti-spoofing and administration. DHCP reservation alone does not prevent another local host from claiming that address. Strong per-device authorization would require additional LAN controls or host identities and is outside this initial gateway-authentication claim.

Ingress source checks reject addresses inconsistent with the receiving LAN or authenticated peer. They do not prove that a trusted gateway has honestly identified every host behind it. A compromised authorized gateway can originate traffic within its permitted source and service scope; the remote gateway must still contain it to that scope.

## 11. Security contract and cryptographic boundary

### 11.1 Threat model

| Threat / actor | Required defense or explicit limit |
|---|---|
| Passive underlay observer | Cannot recover protected inner packet contents under the selected cryptographic assumptions |
| Injector or modifier on the underlay | Invalid packets cannot become delivered inner traffic or change trusted state |
| Replay, duplication, and reordering | Replays are rejected; valid out-of-order packets within the configured window remain usable |
| Impersonator using a known IP address | Must still prove the pinned peer identity |
| Valid peer claiming another peer's source prefix | Source ownership enforcement rejects the packet |
| Valid peer attempting an unauthorized service | Current service policy rejects it |
| Untrusted LAN host | Local ingress validation and service policy limit access; individual host identity is not cryptographically guaranteed |
| Flooding sender | Bounded work, memory, and fair scheduling reduce impact; bandwidth exhaustion cannot be prevented absolutely |
| Attacker dropping all outer traffic | Availability can fail; confidentiality and policy must not fail open |
| Compromised gateway or administrator | Outside the secrecy boundary; can access local plaintext and secrets |
| Compromised service host | VPN does not repair application security or prevent attacks through an allowed service |

The tunnel does not hide outer endpoint addresses, packet sizes, timing, direction, or the fact that gateways communicate. It does not provide anonymity, traffic-analysis resistance, post-quantum protection, or protection from endpoint compromise. Key erasure and forward secrecy are properties to qualify, not conclusions drawn from an unreadable capture.

### 11.2 Required secure-channel properties

The protected profile must provide:

- Mutual authentication against configured public-key identities, fresh session keys, and evidence of possession of the negotiated keys before application delivery is authorized.
- An established authenticated key-exchange construction with fresh ephemeral contributions; no new signature/transcript scheme invented for this project.
- Independent directional protection, authenticated encryption of the entire inner packet, and integrity coverage for all metadata that changes record interpretation.
- Unique nonce use under each key, including concurrent processing, retransmission, restart, and key overlap.
- Replay rejection independent of encryption, with state scoped to one session and direction.
- Forward secrecy for completed sessions to the extent provided by the selected construction and correct secret erasure.
- Authentication of control actions that change established state; network errors and unauthenticated messages cannot revoke trust, change policy, or reset replay windows.
- Explicit protocol/version binding and rejection of unsupported combinations; no downgrade to plaintext, weaker protection, or a previous project's format.
- Fresh keys after restart and bounded overlap during rekey, followed by definite old-key deletion.

Identity authentication, packet protection, and traffic policy are distinct responsibilities. This separation is also useful in established IP security architecture; see [RFC 4301](https://www.rfc-editor.org/rfc/rfc4301). This design does not thereby claim to implement IPsec.

### 11.3 Selection boundary: a security contract is not a cryptographic specification

The selected profile is now specified in [TECHNICAL_DESIGN.md](TECHNICAL_DESIGN.md): Noise IK with X25519/ChaChaPoly/BLAKE2s, `snow` 0.10.0 for the exchange, and VPT1's explicitly defined kernel AEAD record layer and key-handoff boundary. This overall document records the required security properties rather than duplicating byte-level fields. Selection does not establish independent security qualification of the composition.

Noise's specification leaves application responsibilities and out-of-order transport handling to the surrounding protocol. VPT1 therefore fixes its own framing, associated-data coverage, nonce/replay rules, confirmation/rekey exchange, and policy binding in the companion. It does not claim compatibility with Noise's default transport representation or another VPN merely because it uses Noise for key exchange. [Noise Protocol Framework](https://noiseprotocol.org/noise.html)

The companion resolves the profile choices; dependency/composition review, vector conformance, and key-handoff/lifecycle evidence still gate the claim of S. If the selected realization fails that review or cannot meet the requirements, revise it explicitly rather than improvising an insecure fallback.

This separation keeps the overall network design readable while placing the now-selected byte-level protocol in its technical specification. Section 24 distinguishes resolved technical choices from unverified deployment facts and qualification.

### 11.4 Provisioning, rotation, and revocation

Identity private keys remain on their owning gateway with restricted access and no routine inclusion in logs, repository files, captures, or exported status. Public-key distribution is separate from private-key storage. A gateway cloned from an image must receive its own identity rather than duplicate another gateway's private key and live state.

Session key rotation and identity-key replacement are different operations. Session rekeying retains the same configured identity and permissions. Identity replacement is an administrative trust change requiring verification of the new key. The baseline uses an explicit cutover, not an unlimited period accepting both identities.

Revocation blocks new and existing traffic at the gateway applying it, removes eligible sessions and associated flow permission, and prevents outstanding work from reinstalling old keys. Removing peer A on B must not stop B–C. Distributed revocation is not instantaneous: to enforce it at all sites, each relevant administrator must apply it there. A side that has not received an update cannot be assumed to know it.

The enforcement boundary is the gateway's final authorization decision for the affected traffic. After the local policy change commits, no affected packet may receive a new authorization, and work still owned by the tunnel is dropped or revalidated. Packets already admitted into downstream network queues or transmitted beyond that decision cannot necessarily be recalled and may arrive later. Evidence distinguishes those pre-change admissions from traffic incorrectly admitted after the change; observed arrival time alone is insufficient.

## 12. Session and connection lifecycle

### 12.1 Separate status dimensions

Report at least these distinct facts: administrative enablement, configuration validity, underlay reachability, authentication/session state, path-size validity, and observed application-service reachability. A valid key exchange does not prove that the HTTP server is running. A bound UDP socket does not mean the tunnel is ready.

**Operational eligibility for application data, including packets of existing flows,** requires all of: enabled peer, valid policy, permitted flow, valid source ownership, an authorized confirmed key generation, usable path budget, and permitted current lifecycle state. A retiring generation is eligible only for receive during its defined overlap. Any missing prerequisite denies that data. Bounded key-exchange, liveness, and path-validation control traffic has its own restricted admission rules so recovery does not depend on already-authorized application data.

### 12.2 Relationship states

| State | Meaning | Application-data behavior |
|---|---|---|
| DISABLED | Administrator has disabled the relationship | Deny; no handshake initiated |
| INVALID | Required configuration or platform precondition fails | Deny; expose the reason |
| IDLE | Configuration valid; no confirmed session | Deny; designated initiator may establish a session |
| NEGOTIATING | Bounded authenticated exchange in progress | Deny; no early application data |
| ACTIVE | Confirmed current session and usable path | Permit only current authorized flows |
| REKEYING | Current session still valid; replacement in progress | Continue with valid current session within its limits |
| UNAVAILABLE | Path failed, liveness deadline exceeded, or no usable key remains | Deny; bounded recovery attempts allowed |
| REVOKED | Trust or ownership removed | Deny; invalidate sessions and associated permission |

`REVOKED` does not return to `IDLE` because a remote packet arrives. Only a new validated administrative trust decision may re-enable it. A stopped or restarted gateway begins without reusable live session keys or application connection permissions.

The principal transitions are: valid enablement leads to IDLE; an allowed establishment attempt leads to NEGOTIATING; confirmed establishment with a validated path leads to ACTIVE. A soft key limit leads to REKEYING; success returns to ACTIVE, while failure retains only the still-valid current generation. Hard expiry, declared liveness loss, or an invalidated path leads to UNAVAILABLE and blocks application traffic. A bounded failed initial attempt returns to IDLE with backoff or exposes UNAVAILABLE when a path failure is established. Administrative disablement, invalidation, or revocation overrides all active/pending states. Late completions from a superseded configuration or peer incarnation cannot reactivate it.

### 12.3 Establishment and duplicate control traffic

Exactly one configured establishment initiator exists per pair in the baseline: A initiates A–B, and C initiates C–B. This role does not itself determine which side may initiate application flows; Section 9 does that. A configuration with inconsistent initiator roles is invalid. A responder can become ready first and wait without granting access.

The designated initiator also coordinates replacement exchanges. Either side can reach a sending/receiving key limit first, so the responder must be able to cause a bounded authenticated rekey request or notification through the selected protocol. Such a request does not authorize a second conflicting exchange or reset any limit. If the initiator is unavailable or replacement is too late, the limiting side stops at its hard bound. The detailed profile must show that this role assignment can satisfy both directions' lifecycle requirements.

Negotiation attempts and retained handshake state are bounded. Retransmission is idempotent under the chosen secure-channel specification: duplicates do not allocate unlimited sessions, reset nonce state, or repeatedly install keys. Timeouts use monotonic elapsed time and bounded backoff. Failure to confirm keys leaves the relationship non-operational.

Before return-path reachability is validated, response size and rate are constrained so spoofed requests cannot induce large reflected responses or unlimited expensive work. The exact mechanism belongs to the selected protocol profile; trusting a UDP source address is not sufficient validation.

The cryptographic profile must resolve lost final confirmations without treating unauthenticated data as confirmation or deadlocking both sides. The system requirement is mutual peer authentication and the required key-possession evidence before application delivery, not a homemade extra handshake message bolted onto an otherwise reviewed protocol.

### 12.4 Rekey and hard expiry

Per relationship, the design permits a current generation with separate transmitting/receiving keys, at most one retiring receive generation, and at most one pending replacement exchange. Each direction/generation has independent limits and sequence/replay state. Confirmation and activation may complete at different times locally; the selected protocol must ensure that the remote receiver is ready before a sender uses the replacement. This is a required property, not an assumed timing coincidence.

The pending generation cannot carry application data before confirmation. On a confirmed switch, the sender stops producing new records under the previous generation. The receiver may accept valid old in-flight records only during a finite overlap and only until the old generation's hard limit, whichever occurs first. The new generation must never reset the old generation's replay state.

If another replacement is needed before a retiring generation drains, retirement of the older receive generation occurs first or the replacement waits within the current key's valid limits. The number of retained generations never grows to preserve availability; valid delayed old packets may consequently be lost. Hard lifetimes are enforced against each endpoint's local monotonic time and direction-specific usage limits, without assuming synchronized clocks or knowledge of the remote sender's private counters.

Rekey is triggered early enough to precede time, packet, byte, or cryptographic usage limits. If replacement fails, the current generation may operate only while still valid. At hard expiry, traffic stops. Counter exhaustion never wraps and never causes reuse under an old key.

A successful rekey normally preserves authorized inner TCP flows; it does not promise lossless packet delivery. A declared UNAVAILABLE condition, restart, revocation, or relevant policy tightening invalidates affected flow permissions in this baseline. Brief packet loss that does not cross a declared failure condition can be recovered by ordinary inner TCP. Once permission is discarded, applications must make new connections; existing sockets may instead observe timeout until they do so.

The secure-channel profile must distinguish an authenticated rekey with continuity from a fresh establishment after lost state. A replacement without that authenticated continuity clears affected flow permission at the receiver as well. Otherwise a quickly restarted peer could inherit cached connection permission before a liveness timeout notices its restart.

### 12.5 Replay and concurrency invariants

Every accepted record must belong to a valid receiving generation and be accepted at most once. A preliminary replay check may reject obvious old packets cheaply, but unauthenticated data must not advance the authoritative window. Concurrent receivers must not both deliver the same sequence number after independently successful authentication.

Packets outside the finite reorder window are dropped even if otherwise authentic. That is a documented availability tradeoff. Control and data record numbering must use either a shared uniqueness domain or cryptographically separate domains; the final protocol must choose one unambiguous rule.

### 12.6 Outage and recovery

No application payload is kept indefinitely waiting for a session. The baseline drops data that arrives without operational eligibility; ordinary TCP/application retry supplies recovery. Bounded active-transmission queues are allowed, but are flushed or revalidated when identity, policy, session, or path eligibility changes.

Authenticated activity and bounded liveness probes inform peer health. Silence eventually marks the path unavailable according to the recorded profile. Unauthenticated responses cannot extend liveness. When service is otherwise idle, liveness traffic is bounded and backed off during failure.

Receiving authenticated traffic proves only the peer-to-local direction. A healthy bidirectional-path claim additionally requires recent correlated authenticated round-trip evidence, initially supplied by establishment and subsequently by the chosen liveness mechanism. One-way data alone cannot indefinitely hide failure of the other direction. Liveness is local evidence, not a guarantee that every future packet will be delivered.

Recovery establishes fresh authenticated state and validates the path again. It cannot automatically change the peer endpoint, broaden an allowlist, reuse pre-restart counters, or forward queued packets that no longer match policy.

Seamless host suspend/resume, virtual-machine checkpoint restoration, and cloning of live gateway state are outside this lifecycle profile. Such a transition must invalidate session and flow eligibility before forwarding resumes and require fresh establishment with a valid random source; it cannot be treated as ordinary rekey or restored replay state. An environment unable to detect or safely prevent these transitions must exclude them from supported operation. Wall-clock adjustment alone is not the same event and does not justify resetting counters or extending key validity.

## 13. Configuration, startup, shutdown, and control-plane failure

### 13.1 Configuration object

The declarative configuration must identify the local site, LAN and underlay interfaces, local identity reference, peer identity, outer endpoint, designated initiator, peer-owned prefixes, service rules, permitted address families, selected security profile, path budget, and resource/lifetime profile.

Runtime state such as session keys, replay windows, learned liveness, connection state, and counters is not trusted persistent configuration. Restoring a configuration backup does not restore an old active cryptographic session.

The configuration revision must be visible in status and evidence. Unknown fields or unsupported settings are rejected rather than silently ignored. Secrets are referenced through protected storage, not exposed by a normal configuration dump.

### 13.2 Validation and application

Reject duplicate peer entries for one identity within the same gateway configuration, conflicting prefix ownership, invalid addresses, unsafe route recursion, unspecified essential limits, unsupported algorithms/families, unavailable interfaces, invalid service ranges, and incompatible pair roles. Repeated use of B's identity in A's and C's separate configurations is valid. A local validator can check its own role declaration, while an incompatible remote role becomes a visible establishment failure; the system must not claim local validation has inspected the remote configuration. An invalid update leaves the previous valid configuration active, unless the administrator explicitly disables it.

Changes must be transactional from the forwarding perspective even if multiple operating-system facilities are involved. There must be no interval with routes enabled and policy absent. When a fully atomic switch is unavailable, affected forwarding is temporarily denied while the change is applied and verified. A failed update restores the prior valid state or remains safely blocked with a clear error.

A tightening policy takes effect before old cached flow permissions can bypass it. Prefix ownership reassignment, key changes, and peer replacement invalidate affected session and flow state. Expanding a permission requires complete validation; failure to install all necessary protections must not leave a partially broader policy.

Administrative disable/revoke is independently available and does not depend on successfully installing a replacement peer or expanded policy. A failed replacement reports that the old configuration remains active; it must not be reported as a successful revocation. Ordinary service-rule edits invalidate affected flow permission without requiring unrelated peer sessions to restart. After a deny rule is later relaxed again, the old flow does not recover permission from its cached tuple: a new permitted initiation is required.

### 13.3 Startup and teardown invariants

The boot/network configuration must deny unintended underlay↔LAN and cross-site forwarding before any VPN-specific route is made usable. Protection cannot depend on a healthy daemon having run once after boot.

Startup first establishes a safe blocked state, then valid policy and routes, and only then makes eligible authenticated sessions usable. Shutdown blocks new protected forwarding and invalidates affected permission before sessions/interfaces/routes are removed. Persistent private-destination protection survives a normal stop, daemon crash, and missing virtual interface.

The long-running lifecycle controller maintains bounded supervision of data-plane eligibility. If it crashes or loses its administrative relationship with the kernel, an explicit finite supervision deadline closes affected forwarding and invalidates sessions. During the detection interval, only previously valid policy and unexpired authenticated keys can be used. No unbounded orphan session remains active. This is a team-selected operational rule; it is not inherent in kernel networking. Ending an ordinary CLI/status invocation is not a controller failure. A separate exchange worker failing to rekey is governed by current-key validity and hard expiry, unless the supervising controller itself is also lost.

Administrative removal of the VPN is different from disabling it. Removal may withdraw its persistent guard only as an explicit network reconfiguration that explains the resulting routes and exposure. An ordinary “stop VPN” action must not silently reopen a path.

### 13.4 Local management scope

Gateway administration is permitted from a defined local management attachment or local console. The VPN does not expose a control API on the underlay or through remote service access. Only authorized local administrators can change trust, routes, rules, or keys. A compromised administrator remains outside the threat model; the design does not pretend to constrain a root user who deliberately disables all enforcement.

## 14. MTU, fragments, errors, and packet representation

### 14.1 Complete size budget

Let:

- `P` be the verified maximum outer IP packet size on a particular path.
- `H_ip` be the outer IP header size; baseline IPv4 without options contributes 20 bytes.
- `H_udp = 8` bytes.
- `H_record` be the selected tunnel record metadata size.
- `E_crypto` be cryptographic expansion, including transmitted tags/nonces where applicable.
- `Pad_max` be any maximum explicitly permitted on-wire padding.
- `O_max = H_ip + H_udp + H_record + E_crypto + Pad_max`.

The permissible inner packet limit must satisfy **`M ≤ P − O_max`**. Both directions are accounted for; asymmetric paths may have different limits. For a shared virtual interface, its advertised MTU is no larger than the minimum supported limit across active peers unless peer-specific route MTUs are deliberately supported and qualified.

The baseline chooses one shared inner MTU per gateway VPN interface: the minimum safe limit across its enabled, qualified peers. Adding a peer with a smaller budget cannot silently retain the old larger MTU. The change requires a validated local update and the ordinary inner MTU/error behavior for existing flows. A path that cannot carry the selected profile's minimum supported inner/control records is rejected. UDP/IP length limits and the maximum record size also constrain `M`; a large underlying link MTU does not override them.

No fixed `M`, header size, or compression claim is inherited from the previous project. The technical companion now selects a 72-byte total data expansion, giving `M = 1428` for a qualified `P = 1500` path. Its complete budget includes the encrypted record prefix and authentication tag as well as the outer IP/UDP and visible record fields.

Ethernet/Wi-Fi framing, retransmissions, and medium access add separate costs beyond this IP-packet budget. TCP/IP application headers reduce useful application payload further. Compiler structure padding is not a network requirement. Every transmitted field or padding byte needs a documented purpose, and the final profile must provide an exact worst-case budget.

### 14.2 Baseline path-size policy

The baseline uses an administered, verified underlay path budget, not universal automatic PMTU discovery. Endpoint interface MTU alone is insufficient: the intervening path must support the declared size in both directions, including control messages. A profile with unknown overhead or unverified path capacity is not operationally ready.

No outer IP fragmentation is relied on; outer IPv4 packets request non-fragmentation. Fragmented outer delivery is not a supported prerequisite for successful operation. Any operating-system processing/reassembly of hostile outer fragments before tunnel authentication remains subject to the platform's global resource bounds and does not grant peer trust. Known oversize outer sends and credible observed path-size changes invalidate the affected path budget. The relationship then suspends application data until a safe budget is revalidated; bounded control traffic for recovery remains separately permitted. A forged ICMP message may cause a bounded availability disturbance but must not authorize traffic or increase the accepted MTU.

A static qualified budget does not detect every later path change. If a smaller path silently drops large packets while small control packets still pass, the tunnel can exhibit a size-dependent black hole without declaring the peer dead. Suppressed ICMP cannot be assumed detectable by liveness alone. The baseline handles this as a violation of its fixed-path assumption: report the observed failure, suspend/revalidate when unsafe sizing is established, and make no promise of automatic black-hole recovery. Arbitrary dynamic paths require the separate discovery design below. Recovery control messages must themselves fit an explicitly qualified control budget; declaring data unavailable does not make oversize handshake traffic safe.

Dynamic probing and packetization-layer PMTU discovery are possible extensions. If adopted, they require their own complete probing, black-hole detection, and recovery rules; they are not implied by this fixed-path baseline. [RFC 8899](https://www.rfc-editor.org/rfc/rfc8899)

### 14.3 Inner oversize and fragmentation

For inner IPv4 packets larger than the usable tunnel limit:

- With DF set, the forwarding gateway drops the oversized packet and returns the appropriate bounded ICMP fragmentation-needed error with a usable inner MTU, where permitted by normal ICMP rules.
- With DF clear, normal IPv4 fragmentation may produce inner IP fragments that each fit. Each fragment is carried in a separately authenticated record. There is no additional custom VPN fragmentation/reassembly protocol.
- The sender must not silently truncate a packet or clear DF to hide a size problem.

The design requires correct IP-layer error handling, including validation of relevant outer error quotations before deriving inner MTU information. TCP MSS adjustment may be an explicitly documented supplementary measure; it is not a substitute for correct packet sizing and ICMP behavior. [RFC 1191](https://www.rfc-editor.org/rfc/rfc1191.html)

Fragment policy must not create a port-filter bypass. A non-initial fragment does not contain a transport port and cannot independently match an HTTP allow rule. The security-enforcement boundary uses bounded packet normalization/reassembly to obtain a complete valid datagram for transport-policy decisions; generic router forwarding alone does not provide this inspection. At the receiver, outer authentication and peer source checks happen before accepted inner fragments enter that peer-scoped processing. Gateway-produced fragmentation after a complete packet passes policy retains authorization only for that packet's valid fragments, subject to current eligibility at transmission.

Overlapping, inconsistent, excessive, incomplete, and timed-out fragment sets are rejected; state and memory are bounded per peer and globally. Fragment association must include authenticated peer context so different peers cannot combine fragments into one accepted packet. Reassembled traffic undergoes current policy before LAN forwarding and any necessary ordinary IP re-fragmentation. If the selected kernel integration cannot meet this contract, fragment support must be explicitly narrowed in a revised scope rather than silently permitting uninspected fragments.

This normalization is a deliberate firewall/tunnel security function, not a claim that ordinary IPv4 routers must reassemble transit packets. It must not cause an extra IP hop or grant permission to otherwise denied traffic. Tunnel sizing and reassembly introduce real cost and denial-of-service exposure and must be qualified explicitly. [RFC 4459 — Packet Size Issues in Network Tunnels](https://www.rfc-editor.org/rfc/rfc4459.html)

### 14.4 Inner validation and local protocol limits

Both gateways validate inner packets before granting forwarding permission: address family, IP header and total lengths, supported protocol, checksum requirements, and permitted addresses. On tunnel receive, network-supplied content becomes eligible for inner-stack processing only after authenticated record validation and source checks. Truncated or inconsistent packets are rejected. Baseline IPv4 options are unsupported in both directions and must be rejected explicitly, including source-routing options. Loopback, multicast, broadcast, link-local, and underlay-directed inner traffic is denied by the profile.

The inner IP total length must match exactly one carried packet; extra bytes cannot smuggle another packet or be silently ignored. Explicit record padding, if selected, has separately validated framing and is not part of that inner length. Outer UDP checksums are enabled and invalid checksums are rejected; AEAD does not replace basic UDP delivery checks. Inner checksum/offload handling remains correct for ordinary TCP, UDP, and ICMP traffic. [RFC 8085, checksum guidance](https://www.rfc-editor.org/rfc/rfc8085.html#section-3.4)

The kernel handles ordinary local ARP and network-layer forwarding. The tunnel does not create remote Ethernet frames or tunnel neighbor broadcasts. TTL-expired and unreachable errors follow permitted, rate-limited router behavior; invalid unauthenticated outer records do not elicit inner replies.

### 14.5 Datagram, ordering, and congestion semantics

One accepted inner IP packet or inner fragment corresponds to one protected data record in one UDP datagram. Control records are separately typed. The baseline provides no packet aggregation, compression, tunnel-level data retransmission, delivery ordering guarantee, or reliable stream emulation. Inner TCP and applications handle ordinary loss and reordering.

UDP does not supply congestion control. This IP-carrying tunnel relies on the admitted inner transports to respond to congestion, with no second bulk-data congestion controller imposed by the tunnel. Baseline bulk traffic is TCP; nonresponsive UDP tests remain confined to the administered lab under declared offered-load limits. Control/retry traffic is independently bounded and backed off. Per-peer scheduling and queue caps protect resources, while a fixed rate cap is not general Internet congestion control. Public-network qualification must justify both admitted inner traffic and control overhead under the UDP-tunnel guidance. [RFC 8085, UDP tunnels](https://www.rfc-editor.org/rfc/rfc8085.html#section-3.1.11)

Outer DSCP defaults to best effort rather than trusting a LAN application's priority marking. Inner DSCP is not an authorization token. ECN encapsulation/decapsulation must use a defined standards-based behavior and preserve valid congestion indications rather than blindly overwriting ECN bits. The final technical profile must select and test the applicable behavior. [RFC 6040](https://www.rfc-editor.org/rfc/rfc6040.html)

## 15. Failure and recovery contract

| Condition | Required immediate behavior | Recovery / observable result |
|---|---|---|
| HTTP server stopped | Network may remain ACTIVE; application access fails | Distinguish service failure from tunnel failure using local service evidence |
| Missing return route | Requests may arrive but replies fail | Report routing evidence; do not auto-add inner SNAT |
| Wrong/untrusted identity | No operational session; no delivered application data | Administrative trust correction, never trust-on-first-use |
| Underlay unavailable | Data cannot arrive; eventually mark UNAVAILABLE | Bounded backoff and fresh confirmed recovery |
| Gateway restart | Drop old sessions, replay state, and affected flow permissions | Fresh keys and new application connections |
| Control process crash | Preserve guards; close eligibility by supervision deadline | Validated service restart and fresh establishment |
| Rekey failure | Use current keys only while still valid | Stop at hard limits; no extension to preserve availability |
| Peer revoked | Deny affected new admissions and invalidate tunnel-owned work when the local change commits | Explicit re-provisioning required; already emitted packets cannot be recalled |
| Policy narrowed | Affected flows cease despite cached state | New traffic must match new policy |
| Route/interface disappears | Protected destinations remain denied | Revalidate route, interface, and path before resuming |
| Outer endpoint changes | Do not roam or learn replacement automatically | Explicit configuration and fresh authentication |
| Path MTU is observed to become smaller | Stop data using the now-known unsafe budget | Revalidate safe data/control budgets; surface reason |
| Smaller path silently drops large packets but passes control traffic | Large application packets may fail while small packets work; liveness alone may not detect this | Outside fixed-path assumptions; diagnose and explicitly revalidate, without claiming automatic black-hole recovery |
| Invalid record/tag/version | Drop without delivery or trusted-state mutation | Count reason with bounded diagnostics |
| Duplicate/stale record | Drop without repeated delivery | Other valid traffic continues |
| Queue/flow/fragment limit reached | Drop/admit-deny within declared policy | Count exhaustion; reclaim on bounded expiry |
| Peer flood | Contain allocation and scheduling impact where possible | Preserve independent peers within available shared capacity |
| Duplicate/overlapping prefix configuration | Reject update | Previous valid state or explicit safe block remains |
| Clock adjusted | Do not extend lifetimes through wall-clock reversal | Monotonic timers retain correct expiry behavior |
| Unsupported kernel/environment | Do not claim K or activate an unsafe substitute | Explicitly qualify another environment or revise design |

Remote unauthorized traffic is normally dropped without detailed replies that expose policy or amplify traffic. Authorized local traffic with no usable protected path may receive a rate-limited unreachable error where IP rules permit; otherwise the application observes timeout. The hard guarantee is denial of an unauthorized alternative path, not one universal application error string.

## 16. Kernel-processing and platform contract

Profile K means that normal inner packet forwarding, peer selection, protection, encapsulation, receive authentication, replay handling, and reinjection occur in the Linux kernel. Sending every data packet through a userspace process and calling the result a kernel VPN is not acceptable. Userspace configuration, key exchange, status, and experiment orchestration remain compatible with K.

The proposed integration is a routed virtual network interface with the normal Linux forwarding stack. Its exact driver API, concurrency primitives, and key installation mechanism belong to technical design. Correct buffer ownership, execution context, queue behavior, and teardown are prerequisites; moving code into the kernel is not itself a correctness argument. [Linux network-device documentation](https://docs.kernel.org/networking/netdevices.html)

The platform contract requires:

- Ordinary host networking remains usable outside the explicitly controlled routes and firewall policy.
- Packet processing uses bounded resources; one slow or malicious peer cannot allocate indefinitely or hold unbounded global work.
- Checksum and segmentation offload behavior is correct. Captured large software packets must not be mistaken for actual oversized wire packets.
- A configuration change, key deletion, peer removal, interface shutdown, or module unload cannot race with processing to deliver unauthorized traffic or access freed state.
- The system handles packet loss and allocation failure without crashing the kernel or entering an unsafe forwarding mode.
- No unsupported offload, private kernel modification, or hardware acceleration is silently assumed.

The initial supported environment is a qualified Linux test system with network-namespace and veth support. The host's exact distribution, architecture, kernel/configuration, module/loading policy, crypto providers, virtualization if any, and relevant offload settings must be recorded. A namespace is not a userspace network-stack substitute: actual kernel tunnel processing can establish K on this host. The VPN's interfaces, sockets, peer/configuration state, policy, and teardown must support multiple gateway namespaces concurrently without accidental global-state sharing.

Namespace destruction is a separate lifecycle case from stopping a peer process. Removing a namespace's administrative name alone does not guarantee destruction while processes, sockets, or other references retain it. A completed gateway-removal operation must first make that instance ineligible and invalidate pending work, then release its resources without affecting other gateways. A recreated namespace with the same name is a new incarnation and cannot inherit session/flow state from the old one. Loading or unloading a kernel module and a kernel crash are host-wide events, so those are not independent-gateway restart tests. A userspace fallback can establish appropriate C/S behavior but cannot substantiate K.

The later Pi environment has a separate qualification manifest including board model, image, NIC/radio/driver details, and AP and cable behavior. The selected platform is now Linux 6.18.y on x86-64/ARM64, including Pi 5 vendor kernels with 16 KiB pages; this supersedes 6.12.y. The first inspected candidate is 6.18.34+rpt-rpi-2712 on Pi B, not a qualified deployment. [The platform migration contract](docs/PLATFORM_PI5.md) records exact-build, power, tool, management-isolation and radio follow-ups. Existing protocol/policy limits remain in force. No Pi OS release or kernel is supported merely because namespace tests pass or the old project used it. A kernel update requires compatibility review and affected requalification. Initial functional progress does not depend on Pi availability; later physical claims require evidence on the physical setup.

## 17. Independent multi-peer behavior

### 17.1 Minimum meaningful topology

```text
host-a -- gw-a ===== independent A–B session ===== gw-b -- host-b service
                                                    |
host-c -- gw-c ===== independent C–B session =========+

Initial roles are separate namespaces. No A–C transit through gw-b.
No group session or shared peer identity.
```

B terminates two separate relationships. It forwards accepted A or C traffic to its own service LAN. It is not a general transit router between spokes. End-to-end A–C protection through B is therefore neither needed nor claimed.

Initial multi-peer evidence uses `gw-a`, `gw-b`, and `gw-c` as three independently keyed gateway namespaces with separate endpoint/session state and separate host LANs. This is the selected initial test arrangement, not a fallback requiring a third physical Pi. Two host namespaces behind Gateway A alone do not satisfy M. Shared-host resource and failure limits must remain visible; namespace evidence is distinct from the physical demonstration.

### 17.2 Required independence

- A and C may access B concurrently only through their respective explicit rules.
- A session and key are never reused for C, even if the permissions happen to be identical.
- B's destination-prefix ownership and incoming source checks distinguish A from C unambiguously.
- A's replay, handshake, packet queues, and connection state cannot overwrite or advance C's state.
- Revoking, restarting, or rekeying A does not require restarting C's session.
- A cannot request C's routes or use C's inner source addresses.
- B rejects forwarding from an authenticated A ingress to a C tunnel egress, even if Linux has routes to both prefixes.
- Per-peer accounting and scheduling isolate avoidable resource monopolization. Shared CPU, memory, and link capacity remain real shared limits.

The minimum functional qualification is two concurrent remote peers at B. Any higher scale claim needs measurements at that scale. A configurable maximum larger than two is a resource cap, not proof of performance or correctness at that peer count.

### 17.3 Policy change examples

| Administrative action | Intended effect |
|---|---|
| Add A's second host `.11` to the HTTP rule | New host can use A's existing relationship; no additional VPN identity is created |
| Add Host B2 and select the multi-host extension matrix | A2 gains explicit access to B2; A and C do not gain that permission automatically; peer identities and prefix ownership remain unchanged |
| Add C with its own identity and prefix | B gains a separate relationship and explicit C service permission |
| Revoke A on B | A loses access; C's valid service access continues |
| Remove C's HTTP rule but retain peer trust | C may authenticate but cannot use the service |
| Reassign a prefix to another peer | Validate unique ownership and invalidate affected old session/flow permissions before reuse |
| Enable A–C transit | Rejected as out of baseline scope; requires a separate routing, policy, and trust design |

## 18. Operational limits, observability, and diagnostics

### 18.1 Finite operating profile

The finite values are selected in [TECHNICAL_DESIGN.md](TECHNICAL_DESIGN.md), Sections 7–8 and 15–16. The table below defines their overall meaning and constraints. Missing essential values still make a configuration invalid; qualification must show that the selected profile is feasible on the actual hardware, rather than treating its limits as measured performance promises.

| Parameter | Required interpretation / constraint |
|---|---|
| Configured peer limit | Bounds trust/session tables; excess peers are rejected |
| Pending exchanges per peer/global | At most one pending replacement per configured peer; additional global work is bounded |
| Unauthenticated processing budget | Limits packet work, responses, and temporary state before expensive authentication |
| Queue packet and byte limits | Both bounds apply, per peer and globally; exhaustion drops new work with accounting |
| Active flow limit and expiry | Bounds pending TCP, established TCP, UDP, and diagnostic associations; idle expiry or state loss requires fresh permitted initiation |
| Fragment count, byte limit, and lifetime | Bounds incomplete reassembly independently of normal flow limits |
| Replay-window size | Finite reorder tolerance; never reset within a key generation |
| Handshake deadline and retry bounds | Each attempt terminates; enabled peers may keep making bounded, backed-off recovery attempts; concurrent work remains capped |
| Liveness interval and deadline | Deadline exceeds expected probe/reply scheduling budget; expiry closes eligibility |
| Control supervision deadline | Finite maximum unsupervised operation; measured, not assumed instantaneous |
| Key soft/hard time and usage limits | Soft limits precede hard limits; cryptographic maximums take precedence |
| Retiring-key overlap | Finite and never extends hard expiry; at most one retiring receive generation |
| Log and ICMP response budgets | Rate-limited to prevent flood amplification and disk exhaustion |
| Verified path budget | Covers worst-case data and control record sizes in each direction |
| Offered-load limits | Explicit for controlled UDP experiments and shared testbed capacity |

The profile must demonstrate that these values are jointly feasible. For example, a retry deadline longer than a hard key limit cannot justify continued use of expired keys; a large peer count cannot be configured if its reserved resources exceed the gateway's bound. Per-peer bounds roll up to a gateway/namespace bound, and the initial shared-host testbed also has an aggregate allocation ceiling across its gateways; “global” must identify which scope is being measured. State exhaustion denies new admission instead of falling back to stateless service access. All claims of bounded recovery refer to these recorded values and scheduling conditions, not to unqualified hard-real-time guarantees.

### 18.2 Required status and counters

Status must identify the gateway, configuration revision, peer identity fingerprint, configured endpoint, permitted prefixes, administrative state, session generation identifiers without secrets, last authenticated activity, expiry/rekey condition, usable path budget, and reason for non-operational state.

Counters should distinguish:

- Inner packets/bytes admitted, delivered, and denied.
- Outer packets/bytes transmitted and received, including control traffic separately.
- Policy, wrong-source, unknown-peer/session, malformed-record, authentication, replay, and unsupported-family/protocol drops.
- Handshake attempts, confirmed sessions, timeout, rekey success/failure, and hard expiry.
- Queue, flow, and fragment resource exhaustion; MTU/path failures; liveness changes.
- Per-peer and aggregate load, with counter reset epochs.

Counters need defined counting points and byte meanings. Inner bytes are not directly comparable with outer bytes without overhead and control traffic. Local “sent” does not prove remote delivery. A drop reason must not falsely imply that an unauthenticated sender has been identified as a legitimate peer.

Logs expose state transitions and actionable reasons at bounded rates. They exclude keys, raw plaintext, and full application payloads by default. Diagnostic packet capture is an explicit experiment: record interface/direction and protect inner captures as sensitive data. Wall-clock timestamps help correlate machines; monotonic time governs operational lifetimes.

### 18.3 Diagnostic order

An operator should be able to distinguish: local service failure; local host route/firewall failure; wrong remote-prefix ownership; unavailable outer route; failed authentication; policy denial; missing return route; packet-size failure; and overload. Diagnostics should identify the failed prerequisite without recommending that all firewall rules be disabled.

IPv6 deserves explicit attention even though it is not carried. The gateways do not forward IPv6 between LAN and underlay in this profile and do not advertise remote IPv6 reachability. The demonstration uses the fixed IPv4 service address. Alternative IPv6 connectivity on multihomed laptops must be ruled out before interpreting the isolation result. This is not a claim of general host-wide IPv6 leak prevention.

DNS is not required for the primary demonstration. A later private hostname needs explicit resolution and cache behavior; remote multicast name discovery is unsupported. The VPN does not change system DNS or export a resolver by default.

## 19. Performance questions and comparison design

Performance is a measured characteristic within a qualified environment. There is no unsubstantiated minimum throughput or promised improvement over the previous project. The central functional requirement remains successful authorized service use with correct policy and failure behavior.

The hypotheses are: kernel data processing may reduce userspace handoffs; protection and encapsulation add costs; small packets may reach a packet-rate limit; Wi-Fi and queueing may dominate latency; and concurrent peers may expose fairness or contention problems. Each hypothesis needs evidence that distinguishes its cause from alternative explanations.

| Comparison | What it can help establish | Necessary limitation |
|---|---|---|
| Same authorized service path with and without tunnel encapsulation in an isolated measurement fixture | Total tunnel cost relative to equivalent routed forwarding | The deliberately reachable direct fixture is separate from the isolated no-VPN demonstration |
| New kernel path versus a comparable userspace path | Effect of different processing architectures | Match hardware, traffic, protection, MTU, policy, and topology as far as possible; report residual differences |
| Existing VPN on the same testbed | External operational/performance context | Credit it; do not imply protocol compatibility or identical feature/security costs |
| Wired versus wireless underlay | Sensitivity to medium and contention | Keep LAN-side radio conditions visible as well |
| One versus multiple independent peers | Scaling and shared-resource behavior | Report per-peer and aggregate results, not just aggregate throughput |

These are candidate comparisons, selected to answer a stated performance question. They do not require a second VPN implementation or a special plaintext mode solely to populate every row. Any baseline actually used needs its limitations recorded, and lack of a comparable baseline limits the claim rather than expanding project scope automatically.

The original project's measurements may provide historical context, but different application endpoints and routing mean they are not a controlled baseline for the new system. A direct Pi-to-Pi benchmark and laptop-to-laptop service traffic are not equivalent paths.

Initial performance observations come from the namespace testbed and must be labeled as single-host virtual-link results. The gateway and application namespaces compete for the same CPU, memory bandwidth, kernel workers, and scheduling resources; per-namespace process CPU does not cleanly attribute all shared kernel work. Veth/bridge links do not reproduce a Wi-Fi medium or physical NIC behavior. Record host-wide load and any CPU allocation, offload settings, artificial link limits, delay, loss, or MTU constraints. Controlled impairment can exercise failure behavior but is not evidence of actual Pi or wireless throughput. Namespace and physical measurements are reported separately.

Measure application goodput, response/transaction latency distributions, packet rate, end-to-end loss, retransmissions, system CPU including kernel/softirq work, memory, queue drops, and concurrent-peer fairness. Record packet size, MTU, service/workload parameters, direction, duration, repetitions, device temperatures/throttling, offloads, and network conditions.

Use repeated, counterbalanced or randomized comparisons with stated sample counts and uncertainty. Separate warm-up from measurement; report failed runs and exclusion criteria. Throughput, packet-size efficiency, and CPU cost are separate results. Packet captures must account for capture loss and offload artifacts. Process CPU for the configuration daemon cannot represent kernel data-path CPU.

Any plaintext measurement is a deliberately isolated comparison with explicit labeling. It must not create a remotely selectable or automatic unprotected mode in the operational VPN. Performance observations cannot waive security, routing, or policy failures.

## 20. Invariants for design review

These invariants connect the component descriptions into one system. A design or implementation change violating one requires correction or an explicit scope revision before qualification.

| ID | Invariant |
|---|---|
| I01 | The service path traverses both intended gateways; operational S/K/M traffic uses their protected relationship. |
| I02 | A remote-prefix route selects a peer; it never grants service permission by itself. |
| I03 | In S/K/M, every inner packet admitted from the tunnel has valid peer/session authentication, replay acceptance, source ownership, and current flow/service authorization. |
| I04 | Underlay plaintext cannot enter a private LAN as authenticated tunnel traffic. |
| I05 | A protected destination never falls back to plaintext, an unrelated default route, or another peer when protection fails. |
| I06 | The request and response use explicit valid routes without hidden inner NAT. |
| I07 | A reply permission cannot authorize a new reverse-initiated connection. |
| I08 | No key/nonce pair is reused; restart and rollover cannot reset a live key's uniqueness domain. |
| I09 | One valid record is delivered at most once, including under concurrent processing. |
| I10 | Key hard limits, revocation, and policy tightening override cached connection permission and queued work. |
| I11 | One peer cannot claim another peer's addresses, state, or privileges. |
| I12 | Removing/rekeying one peer does not require unrelated peers to lose their sessions. |
| I13 | Every emitted outer packet fits its declared qualified path budget; observed invalidation suspends unsafe data, and inner oversize has defined IP behavior. Silent path changes have the stated fixed-path limitation. |
| I14 | Fragmentation cannot bypass source or service authorization. |
| I15 | Memory, state, retries, queues, and diagnostic output are bounded. |
| I16 | Routine data processing in profile K stays in the kernel; control-process placement does not obscure this claim. |
| I17 | Temporary configuration states are either fully enforced or blocked. |
| I18 | No claim of service health, security, scale, compatibility, or speed exceeds its recorded evidence. |

## 21. Demonstration and acceptance evidence

### 21.1 Evidence package

Each qualification run needs a topology and address manifest, hardware/software profile, redacted configuration revision, peer fingerprints, route/firewall state, service binding and local health evidence, timestamps, packet-path captures, relevant counters, application results, and a statement of which profile and invariant were tested.

For initial runs, the manifest includes namespace names/identities, veth endpoint ownership, underlay bridge membership, application and tunnel-socket placement, per-namespace routes/firewall settings, and the shared host/kernel configuration. Captures and counters identify the namespace and interface. Demonstrate that ordinary host traffic has no root-namespace NAT/proxy or bridge shortcut to the service. The same host kernel and clock are explicit properties of this testbed, not evidence of isolation between separate operating systems.

Use a fresh request identifier in the same HTTP service response and record the server's receipt of it. Disable application caching or use a fresh uncached request, and remove proxy ambiguity. A saved page, cached DNS answer, or old TCP response is not proof of current reachability.

An underlay capture should show the gateway-to-gateway carrier; LAN-side evidence should show the original private client and service addresses. Where physical encryption offloads or capture placement change visibility, document the observation point. The absence of a readable marker is supporting confidentiality evidence, not a cryptographic proof.

### 21.2 Acceptance matrix

The matrix describes evidence for the full proposed scope. A01–A09, A31, A33, and A35 establish the principal connectivity/policy behavior of C, interpreted for the actual declared experiment. The identity/session checks qualify S; A29 qualifies K's actual packet path; A14 and A25–A28/A36 qualify multi-peer isolation and independence. Size, fragment, lifecycle, and resource checks substantiate those advertised system behaviors; A32 supports whatever performance claim is made. These broader checks do not turn security or kernel qualification into a prerequisite for showing the basic private-service use case. Additional rows below target the corresponding specific design properties rather than demanding production certification.

| ID | Scenario | Required result / evidence |
|---|---|---|
| A01 | Access Host B from an ordinary client in `gw-b` over B's LAN, then keep that local control available | Fresh response succeeds independently of the VPN; the same listener remains healthy during remote negative tests |
| A02 | Same A client and B service with VPN disabled | Cross-site access fails while local service still works; no alternate route/proxy explains the result |
| A03 | Enable valid A–B relationship | Same private address and service succeed with a fresh request identifier |
| A04 | Trace allowed request and response | Both LAN observations and underlay carrier agree with Sections 6–8; server sees A's original address |
| A05 | Stop tunnel during access, then attempt a fresh request | Protected access fails; no plaintext/private-service traffic escapes on another route |
| A06 | Attempt disallowed port, host, and unapproved local client | All denied with attributable policy evidence; negative targets have known-working listeners/reachability controls so a missing service is not mistaken for policy enforcement |
| A07 | Start a new B→A connection, including source port 8080 | Denied while legitimate A→B replies still work |
| A08 | Send direct underlay traffic toward private LAN destinations | Denied independently of tunnel/session status |
| A09 | Compare unrelated route selections and access a separately known-working local destination before/after VPN enablement | Routes and local reachability remain unchanged; no unintended tunnel/default-route takeover; Internet success is not required in the isolated namespace setup |
| A10 | Change peer key or use an unknown identity | No usable session or delivered application data |
| A11 | Alter authenticated data/metadata | Rejected without accepted plaintext or unauthorized state change |
| A12 | Replay captured data/control; duplicate concurrently | No duplicate delivery or repeated privileged state transition |
| A13 | Deliver valid packets out of order | Accepted within window; older/outside-window packets dropped as specified |
| A14 | Peer A spoofs C or B inner source addresses | Rejected even with a valid A session |
| A15 | Force rekey while an allowed flow is active | Confirmed replacement, bounded overlap, no key/counter reuse; inner flow remains eligible unless ordinary loss disrupts it |
| A16 | Prevent rekey completion until hard expiry | Forwarding stops at the relevant hard limit; no old-key lifetime extension |
| A17 | Restart one gateway; inject pre-restart records | Old traffic rejected; fresh authenticated recovery and new application connection work |
| A18 | Crash control service or remove virtual interface | Persistent guards hold; eligibility closes within the declared supervision bound |
| A19 | Revoke peer or narrow policy during a flow, then relax the service rule | No new affected admission after local commit; distinguish pre-change in-flight packets; stale work cannot restore access and old TCP packets cannot recreate permission after relaxation |
| A20 | Apply invalid/partial configuration update and deliver a delayed handshake/key-install completion from the superseded revision | Previous valid policy or a safe block remains; no transient broader access or late reactivation; failed replacement is not reported as successful revocation |
| A21 | Test packets at, below, and above inner size limit | Correct forwarding, fragmentation or ICMP behavior; no outer fragmentation/truncation |
| A22 | Exercise fragments, including malformed/overlapping/cross-peer sets | Valid qualified fragment behavior works; evasion and resource abuse fail safely |
| A23 | Reduce path size with valid feedback, then separately suppress feedback while allowing small control traffic | Observed unsafe sizing blocks data until revalidation; silent large-packet loss is reported with the fixed-path limitation and is not falsely claimed detectable by liveness alone; no unprotected fallback |
| A24 | Lose/reorder/delay outer packets and block the path | Ordinary application effects are visible; retries/state remain bounded; recovery obeys fresh-session rules when required |
| A25 | Concurrent A and C access to B | Independent identities/sessions/counters and correct shared-service behavior |
| A26 | Rekey, restart, revoke, or overload A while C is active | C's trust/session is not reset; resource effects and available capacity are reported honestly |
| A27 | Attempt A↔C transit through B | Denied even while both service relationships work |
| A28 | Use separate A/A2 client namespaces and B/B2 service namespaces on their respective local LANs | All three allowed combinations in Section 9.4 work concurrently with correct return routes and original sources; A→B2 and reverse initiations fail while both listeners are healthy; B2 permission/service removal leaves allowed B flows working; reported as multi-host evidence, not independent multi-peer evidence |
| A29 | Examine actual normal data path | Kernel path substantiated by configuration/instrumentation; no userspace per-packet relay for profile K |
| A30 | Exhaust queues/flows/fragments or send malformed/flood traffic | Bounded memory/work and categorized drops; no crash, privilege bypass, or unlimited logs |
| A31 | Validate split-routing, DNS, and alternate-family assumptions | No unintended default-route/DNS changes; no IPv6/proxy/secondary-interface path invalidates isolation |
| A32 | Exercise load and compare eligible baselines | Reproducible observations with uncertainty and whole-system accounting; no unsupported causal speed claim |
| A33 | Boot, shutdown, and repeated enable/disable cycles | Guards precede exposure, permissions do not accumulate, old sessions do not revive |
| A34 | Temporarily allow an ordinary UDP service and diagnostic ICMP echo, then remove those rules | Both protocols work within their explicit scope; unrelated traffic and traffic after rule removal remain denied; neither requires a VPN-aware application |
| A35 | Verify namespace placement and link topology with the VPN disabled | Client/service applications occupy their own host namespaces; only gateway outer ports join the underlay; no root-namespace route, NAT, proxy, or shared LAN bypasses the tunnel |
| A36 | Run gateways concurrently with identical local UDP ports; remove/recreate one gateway instance, including a case with retained references to its old namespace | Separate sockets, identities, sessions, policy, and counters; named removal is not mistaken for completed teardown; new incarnation has no stale state; unrelated gateways continue; shared module unload is not an independent peer failure |
| A37 | Lose/expire TCP permission while endpoints retain their sockets; inject midstream packets before and after fresh tunnel establishment | No midstream pickup or reverse-initiated permission; fresh allowed SYN/handshake can establish a new application flow |
| A38 | Generate a legitimate inner router error and legitimate underlay ICMP from a non-peer router; inject unmatched/error-control messages | Necessary associated errors pass the correct validation path, including before TCP completion; unmatched quotations, redirects, and unauthorized gateway-local access grant no permission |
| A39 | Configure A and C with the same pinned B identity and distinct A/C identities | Valid configurations work with independent pairwise keys/state; duplicate peer entries inside one local configuration or a shared A/C identity are rejected |
| A40 | Reach a responder-side soft/hard key limit and separately fail only one underlay direction | Responder can request coordinated replacement or stops at its bound; inbound-only activity does not falsely sustain a bidirectional health claim |
| A41 | Add a peer with a smaller qualified packet budget to a gateway using a shared VPN interface | Shared inner MTU is reduced safely before activation; existing flows receive the defined sizing behavior; data and recovery control traffic obey their respective budgets |
| A42 | Send malformed inner lengths, trailing bytes, unsupported IPv4 options/protocols, invalid checksums, and hostile outer fragment traffic | No invalid inner delivery or policy bypass; UDP/offload behavior is correct and pre-authentication fragment/resource handling stays bounded |
| A43 | Exercise an allowed suspend/resume or VM-restore transition only if the declared platform supports safe detection | Old session/flow eligibility is invalid before forwarding resumes; fresh establishment uses valid randomness; otherwise the profile explicitly excludes the transition rather than claiming seamless recovery |

Security adversarial evidence requires inspection of actual behavior and known expected outcomes; a single negative test or packet capture does not prove an entire cryptographic property. Kernel qualification also needs appropriate memory/concurrency and lifecycle diagnostics. These are required evidence categories, not an implementation task breakdown.

### 21.3 What constitutes a failed qualification

Any unauthorized delivery, plaintext fallback, incorrect peer attribution, replay delivery, reuse of a key/nonce pair, unbounded state growth, kernel fault, or false success caused by an alternate path fails the relevant profile. A slower-than-expected but correct system may still meet the networking objective; it cannot support an improvement claim without measurement.

A missing third gateway means M is unproven. Missing actual-kernel-path evidence means K is unproven. A correct service demonstration with an unreviewed or incomplete secure-channel specification does not complete S. Report partial results precisely rather than collapsing all profiles into “VPN works.”

## 22. Requirements-to-evidence traceability

| Requirement | Governing design | Principal evidence |
|---|---|---|
| R01 private-service access | Sections 5–8 | A01–A04; A28 for the multi-host extension |
| R02 normal applications and return routing | Sections 6, 8–9 | A03–A04, A07, A28, A34 |
| R03 isolation without VPN | Sections 5, 6.4, 13 | A02, A05, A08, A31, A33, A35 |
| R04 Layer-3 split tunnel | Sections 3–6, 14 | A04, A09, A21–A23, A31, A34 |
| R05 least permitted service/initiation | Sections 9–10 | A06–A08, A14, A19, A27–A28, A37–A38 |
| R06 protected transport | Sections 10–12 | Reviewed security profile plus A10–A17, A39–A40 |
| R07 kernel data path | Sections 7, 16 | A29, A36 and recorded platform diagnostics |
| R08 independent peers | Sections 10, 17 | A14, A25–A28, A36, A39 |
| R09 fail-closed behavior | Sections 6.4, 12–15 | A05, A16–A20, A23, A33 |
| R10 lifecycle, size, bounded resources | Sections 12–18 | A15–A24, A26, A30, A33, A36–A43 |
| R11 defensible evidence/performance | Sections 18–21 | Complete evidence manifest and A32 |
| R12 honest scope and authority | Sections 1–3, 24–26 | Decision register, profile-specific qualification report |

## 23. Principal risks and containment

| Risk | Design response | Residual limitation |
|---|---|---|
| Namespace topology accidentally bypasses the gateways | Separate application/gateway namespaces, outer-only underlay bridge, no root-namespace data path, and explicit placement evidence | Namespace isolation does not protect against a privileged operator on the shared host |
| Global tunnel state breaks concurrent gateway namespaces | Require namespace-aware configuration, sockets, sessions, counters, and teardown | A kernel fault or module unload still affects the shared host |
| A Pi's radio cannot host the laptop network (AP mode, channel, driver) | Qualify radio/driver/interface support separately; initial namespace tests continue independently | AP mode ran on one Pi 5 on 4 November 2026; the other Pi is not yet qualified |
| A service appears reachable because of another interface or cache | Single intended path, fresh request IDs, local-server control, route and capture evidence | Evidence is valid only for the recorded test setup |
| Scope expands into custom cryptography | Established exchange and a blocking reviewed-profile requirement | Composition and implementation still need specialist scrutiny |
| Kernel integration is too complex or incompatible | Narrow platform support and require actual path/lifecycle evidence | No automatic portability or speed claim |
| More routes accidentally grant more access | Separate prefix ownership, service policy, and initiation state | Trusted administrators can still misconfigure intended permissions |
| Cached flow state survives revocation | Recheck current authorization and invalidate affected state on committed change | Distributed sites learn administrative updates at different times |
| Fragments bypass port policy | Peer-scoped bounded reassembly and complete-packet policy decisions | Fragment support depends on technical integration satisfying the contract |
| Unknown or shrinking path MTU stalls traffic | Qualified data/control budgets; observed invalidation causes safe suspension/revalidation | Silent size-dependent loss may evade small-packet liveness; automatic black-hole recovery and arbitrary dynamic paths are outside baseline support |
| One peer consumes shared resources | Per-peer/global bounds, fair scheduling, separate state | Shared-link saturation can still reduce everyone's throughput |
| Misleading performance conclusions | Comparable fixture, whole-system measurements, repeated trials | Noise and residual differences must remain visible |
| The design's content is mistaken for implemented capability | Profile status and acceptance evidence kept separate | No paper design eliminates implementation defects |

## 24. Decision register and external confirmations

### 24.1 Proposed decisions made by this document

The baseline uses Linux network namespaces for initial testing, as explicitly clarified by the team. The proposed network design chooses site-to-site routed IPv4, split routing, UDP/IPv4 underlay, non-overlapping prefixes, no inner NAT, explicit TCP/8080 service permission, independent public-key identities, static endpoints, a kernel data plane with a separate control component, pairwise multi-peer relationships without transit, and failure-closed protected destinations. The Pis remain the intended later physical arrangement. Changing these is a design revision, not an incidental implementation detail.

### 24.2 Open questions

| ID | Confirmation needed | Current proposed position |
|---|---|---|
| Q02 | Does the physical demonstration use each Pi's Wi-Fi as the laptop LAN with a cable between the Pis? | Decided 5 November 2026 (user): yes, as in the README's example; the 28 October Ethernet-LAN/Wi-Fi-underlay proposal is withdrawn. It preserved the two private LANs and the service path; it preserves the two private LANs and service path; initial namespace testing is already selected |
| Q03 | What original kernel/protocol contribution should the project include? | New project tunnel/data-path integration with established cryptography, not an existing VPN merely relabeled |
| Q04 | How should kernel processing and independent multi-peer evidence be weighted? | C is the main networking objective; S/K/M are clearly separate intended final claims |
| Q05 | What namespace versus physical evidence is required for the final M demonstration? | Initial M testing uses three gateway namespaces; this decision is already made |
| Q06 | Which performance comparisons and level of measurement are expected? | No claimed speedup; use fair and reproducible comparisons |
| Q07 | Was a separate NAT/Internet deployment expected in addition to the selected direct Ethernet cable underlay? | The direct cable between the Pis needs no NAT traversal, and VPT1 does not claim it |

These questions do not grant permission to silently change the project. They identify where expectations cannot be established solely by technical reasoning.

They are questions for aligning the eventual demonstration, not new approval gates for the already selected namespace testbed or ordinary design work. The connectivity objective remains explainable without resolving every broader team feature first.

### 24.3 Selected technical specification and remaining qualification facts

| ID | Technical item | Selected resolution | Qualification still required |
|---|---|---|---|
| D01 | Secure channel and key interface | Technical Sections 3–8 fix Noise IK, Snow 0.10.0, Split handoff, AEAD framing, confirmation and rekey | Composition/dependency review and actual security conformance |
| D02 | Wire representation and overhead | Technical Sections 4–5/15 fix VPT1 fields, UDP 51888, nonce/AAD/replay rules, and 72-byte data expansion | Parser/vector/interoperability evidence |
| D03 | Platform and hardware | Technical Sections 9/17 select public Linux 6.18.y APIs, including Pi 5 vendor builds/16 KiB pages, x86-64/ARM64, and mandatory facilities | Exact installed kernel builds, provider/module and AP/radio compatibility |
| D04 | Operating limits | Technical Sections 7–8/15–16 select PMTU/MTU, time/key/replay/queue/flow/memory limits | Boundary behavior and measured resource feasibility |
| D05 | Kernel/policy integration | Technical Sections 9–14 fix hooks, owners, zones/witnesses, normalization, synchronization, and teardown | Memory/concurrency and actual packet-path qualification |
| D06 | Initial/final topology and routes | Technical Sections 2/19 fix namespace equivalents and the Wi-Fi LAN–Pi–Ethernet cable–Pi–Wi-Fi LAN physical mapping | Real interface/route/firewall/listener and no-bypass evidence |
| D07 | Independent peers and scale | Technical Sections 2/11/16/19 fix three-gateway evidence, four-peer admission cap, and no transit | M qualification and physical-evidence expectations |
| D08 | Measurement fixture | Technical Section 19.4 selects workloads, trial counts and conditions | Results, uncertainty and justified performance claims |

The technical companion now supplies D01/D02/D04/D05 and the chosen profiles for the other items. Exact machine inventory and successful qualification remain evidence to obtain. This overall document is not the wire-protocol specification and does not imply that the selected technical realization has already passed review or testing.

The decision boundary is explicit: topology, host/gateway roles, permitted resources and initiation, routing/return paths, identities and trust distribution, failure semantics, static endpoint/path scope, namespace isolation, shared-MTU policy, and evidence expectations are decided here as proposals or confirmed team direction. Algorithms, field layouts, kernel APIs, and concrete budgets are selected in the technical companion; deployment commands remain a separate concern. No item may be treated as silently resolved by the old project. If the technical realization cannot meet an overall constraint, revise the constraint openly rather than filling the gap with an unsafe default.

## 25. Completion and change control

Completion of C is reported independently from the broader team target. The following conditions define completion of the full proposed C + S + K + M scope; they do not redefine the minimum networking demonstration or require formal production-security certification:

1. The project direction is reconciled with the explicit clarifications.
2. The initial namespace two-LAN topology meets C, including the disabled/enabled/disabled comparison and the return path; the intended later physical demonstration has separate evidence satisfying the confirmed expectations.
3. The chosen secure-channel profile is fully specified and reviewed, and S has supporting evidence.
4. The qualified Linux environment substantiates K rather than merely launching a kernel-related component.
5. At least three independent gateways substantiate M, including denial of transit and independent revocation.
6. Lifecycle, resource, fragmentation, sizing, policy-change, and failure behavior meet the applicable acceptance criteria.
7. Performance statements, limitations, and supported environments match recorded results.
8. Outstanding limitations are documented without presenting them as completed capabilities.

Future additions—IPv6, Internet NAT traversal, roaming, overlapping networks, remote access, full tunneling, transit routing, or larger scale—must state the new use case, new trust/path assumptions, changed policy, failure behavior, and new evidence. They must not invalidate the existing private-service boundary or silently weaken protection.

## 26. Sources and interpretation

### 26.1 Project sources

- [Current project README](README.md): primary statement of project vision.
- Team clarification on 17 October 2026: initial testing uses Linux network namespaces, not the Pis. This is the selected initial environment; it does not replace the README's intended later physical demonstration.
- User topology decision on 5 November 2026: each Pi hosts a Wi-Fi network for its local laptop and the two Pis are joined by an Ethernet cable. This replaces the 28 October proposal (each Pi connects to its local laptop over Ethernet; Pi A hosts the gateway-underlay Wi-Fi and Pi B joins it), which is withdrawn. The physical profile preserves the two-LAN service objective.
- [Historical code README](../CS331-T005-MinimalVPNTunnel/code/README.md): describes the implemented host-to-host educational tunnel and limitations.
- [Historical architecture](../CS331-T005-MinimalVPNTunnel/code/docs/architecture.md): documents the userspace TUN/UDP packet path.
- [Historical protocol](../CS331-T005-MinimalVPNTunnel/code/docs/protocol.md): documents the 28-byte format and custom handshake; not adopted.
- [Historical main source](../CS331-T005-MinimalVPNTunnel/code/src/main.c), [handshake source](../CS331-T005-MinimalVPNTunnel/code/src/handshake.c), and [common constants](../CS331-T005-MinimalVPNTunnel/code/include/common.h): inspected to ground the comparison in the source tree.
- [Historical report source](../CS331-T005-MinimalVPNTunnel/report/main.tex) and [packet experiment summary](../CS331-T005-MinimalVPNTunnel/report/evidence/20260830-121636-packet/summary.md): measured context and reported limits; not a baseline proof for the new topology.
- [Historical proposed redesign](../CS331-T005-MinimalVPNTunnel/docs/NEW_VPN_DESIGN.md): reviewed as an earlier proposal; its byte counts, scope, and production claims are not inherited.

### 26.2 Primary technical references

References below support specific networking concepts. They do not certify this proposed system or substitute for project-specific references. Applicable updates and errata must be checked when a concrete technical profile is selected.

| Source | Relevance |
|---|---|
| [RFC 1812 — Requirements for IP Version 4 Routers](https://www.rfc-editor.org/rfc/rfc1812.html) | Routing, forwarding, TTL, and ICMP responsibilities |
| [RFC 1191 — Path MTU Discovery](https://www.rfc-editor.org/rfc/rfc1191.html) | IPv4 DF and packet-too-large behavior |
| [RFC 4459 — Packet Size Issues in Network Tunnels](https://www.rfc-editor.org/rfc/rfc4459.html) | Tunnel fragmentation/reassembly tradeoffs and resource risks |
| [RFC 8085 — UDP Usage Guidelines](https://www.rfc-editor.org/rfc/rfc8085) | UDP tunnel limits and congestion responsibility |
| [RFC 8899 — Packetization Layer Path MTU Discovery for Datagram Transports](https://www.rfc-editor.org/rfc/rfc8899) | Reference for a possible later dynamic path-size design |
| [RFC 6040 — Tunnelling of Explicit Congestion Notification](https://www.rfc-editor.org/rfc/rfc6040.html) | Defined ECN propagation rather than ad hoc bit copying |
| [RFC 4301 — Security Architecture for the Internet Protocol](https://www.rfc-editor.org/rfc/rfc4301) | Architectural distinction between protection associations and traffic policy |
| [RFC 5128 — Communication across NATs](https://www.rfc-editor.org/rfc/rfc5128.html) | Outer endpoint mappings and the separate problem of establishing reachability through NAT; background, not a selected traversal mechanism |
| [Noise Protocol Framework](https://noiseprotocol.org/noise.html) | Selected IK exchange framework; VPT1's complete record/lifecycle profile is specified in the technical companion |
| [Linux network-device documentation](https://docs.kernel.org/networking/netdevices.html) | Data-path and device-lifecycle responsibilities requiring concrete technical design |
| [Linux connection-tracking documentation](https://kernel.org/doc/html/latest/networking/nf_conntrack-sysctl.html) | Established-flow pickup is a distinct behavior; flow-tracking defaults cannot substitute for an initiation policy |
| [Linux network namespaces manual](https://man7.org/linux/man-pages/man7/network_namespaces.7.html) | Network-resource isolation in the initial test environment |
| [Linux veth manual](https://man7.org/linux/man-pages/man4/veth.4.html) | Virtual links connecting host, gateway, and underlay namespaces |

The topology, permissions, lifecycle policy, scope boundaries, and acceptance matrix are proposals for this project. They are not quotations from these references and are not represented as external requirements.
