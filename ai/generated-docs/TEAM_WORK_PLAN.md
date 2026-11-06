# VPNTunnel team work plan

**Team:** Aayush, Raj, Harsh, Anvit<br>
**Date:** 31 October 2026<br>
**Status (5 November 2026):** Work assignment and delivery contract. Merged: AY1 to AY3; the AY2 runtime, Netlink endpoint and thin data path; AY4 re-keying without packet loss (by age and by key usage), the controller's silence re-key, kernel-issued attempt IDs and receive locators (`BEGIN_ATTEMPT`) and kernel PING/PONG liveness with a 15 s closure ([AY4_LIVENESS.md](docs/AY4_LIVENESS.md)); AY5 allocation-failure, runtime and churn tests, and sanitizer runs (KASAN, UBSAN, lockdep, kmemleak, KCSAN) on the x86-64 reference VM ([QUALITY_CHECKS.md](docs/QUALITY_CHECKS.md)); the controller that brings a tunnel up and keeps it up (RJ1 to RJ3), live reload of a changed configuration file (RJ4), the validate-differential check, the adversarial control-plane tests (RJ5) and controller timers on the boot clock; HR2, the HR3 normalizer core and its runtime adapter (continuity, shared fragment budget, bounded worker), HR4 checksum/ECN/size checks (#41) and HR5 snapshot bounds (#43); AN1 to AN3; the AN4 acceptance ledger (rows that were not run or only partly covered are marked as such) and the AN5 runbook (its physical results are pending); and the demo kit (gateway script, Wi-Fi access point script, reachability test, Pi checklist, explanation script). Open, awaiting review: HR4/HR5 transactional MTU and post-crypto admission (#66, brought up to date with `main` on 5 November) and, on top of it, the live fragment normalization, associated ICMP error and bounded transmit-batch adapter ([HR3_LIVE.md](docs/HR3_LIVE.md), [HR4_ERRORS.md](docs/HR4_ERRORS.md)); full HR4/HR5 acceptance still needs the non-author reviews, a reviewed live-witness scope and the ARM64/Pi lane. The HR1 perimeter was scrapped on 3 November 2026, see [PERIMETER_REMOVAL.md](docs/PERIMETER_REMOVAL.md). Not done: the encrypted activation handshake (CONFIRM, READY, COMMIT, COMMITTED), REKEY_START/REKEY_OK/REKEY_NEEDED and a kernel-started re-key, live flow witnesses and conntrack zones (the stateless ACL is the live policy), multi-facility transactional updates (RJ4 reloads the configuration but does not coordinate nftables and routes), the multi-peer, long-run and ARM64 rows of AN4's campaigns, and the physical two-Pi run (AN5). The whole kernel suite has not yet been run on the integrated `main` after today's merges. One Raspberry Pi 5 has passed the kernel suite earlier (both gateways as namespaces on that Pi); Pi A and the two-Pi demo have not run. The physical demonstration topology changed on 5 November 2026: each Pi hosts a Wi-Fi network for its laptop and the Pis are joined by a cable ([DEMO.md](docs/DEMO.md)). These slices do not complete the VPN. The x86-64 reference image and fixtures exist<br>
**Repository:** [Aayush75/VPNTunnel](https://github.com/Aayush75/VPNTunnel), integration branch `main`.

## 1. What this plan governs

Build the selected VPT1 design in [TECHNICAL_DESIGN.md](TECHNICAL_DESIGN.md), preserving the objective and acceptance cases in [DESIGN_PLAN.md](DESIGN_PLAN.md). This document assigns implementation responsibilities, dependencies, reviews, and release gates. [CODING_STANDARDS.md](CODING_STANDARDS.md) defines implementation rules; [TEST_PLAN.md](TEST_PLAN.md) defines the executable test contract to build.

Linux network namespaces are the main development and automated acceptance environment. Raspberry Pis are the final hardware and radio validation stage. The final physical arrangement (decided 5 November 2026) is Laptop A—Wi-Fi—Pi A—Ethernet cable—Pi B—Wi-Fi—Laptop B. Namespace tests must already establish the routing, policy, security, and independent-peer behavior before that stage.

Assignments are based on component boundaries, not assumptions about anyone's experience. Every owner implements their component, its focused tests, its diagnostics, and its documentation. Anvit owns the shared harness and evidence system; he is not responsible for discovering and fixing everyone else's bugs. All four must be able to explain an ordinary request and its response through the whole system.

The project cannot promise defect-free software or perfect tests. Its completion criterion is explicit coverage, independent review, reproducible evidence, no unresolved failures affecting the claimed features, and honest limits. No proposed check in these documents is a claim that it has run.

## 2. Primary ownership and review

| Person | Primary responsibility | Deliverables | Main reviewer |
|---|---|---|---|
| Aayush | Linux tunnel runtime and integration | Module/device/socket lifetime; Generic Netlink kernel endpoint; key-generation and replay state; Crypto API path; bounded workers/queues; counters; activation/control handling in kernel; namespace teardown | Harsh for kernel/lifetime changes; Raj for protocol/Netlink changes |
| Raj | Rust control plane and secure exchange | Configuration/identity tooling; Noise IK and validation wrapper; exchange retry/cache logic; one-time Split handoff; Netlink client; controller lease/daemon; userspace unit/conformance tests | Aayush for lifecycle/ABI; Harsh for policy configuration |
| Harsh | Kernel forwarding policy and IP correctness | ACL snapshots; prefix ownership; conntrack zones and witnesses; fragment normalization; MTU/DF/ICMP/ECN behavior; focused policy/IP tests | Aayush for concurrency/lifetimes; Raj for configuration semantics |
| Anvit | Linux test infrastructure, CI, and final demonstration | Reproducible VM images/manifests; namespace fixtures; independent packet oracle; test runner/results; failure injection; CI orchestration; benchmark harness; Pi environment and evidence package | Raj for test oracle/protocol cases; Aayush and Harsh for kernel and network evidence |

Aayush is the integration coordinator, not a sole approver. Harsh is the second kernel maintainer. Raj is the protocol/ABI co-maintainer. Anvit owns release-evidence completeness; he cannot waive a component's failed security or correctness gate. Each owner names one backup in their issue: Aayush↔Harsh, Raj↔Anvit. Backup means ability to reproduce and diagnose, not an automatic transfer of primary work.

## 3. Work packages and acceptance

These are separate GitHub issue-sized packages. Split further when necessary; do not turn one row into a long-lived branch containing an entire subsystem. Package IDs describe responsibilities, not arbitrary completion dates.

### 3.1 Aayush

| ID | Work package | Required handoff evidence |
|---|---|---|
| AY1 | Qualify the external-module API/build and per-net foundation with Harsh | Exact Linux 6.18.y builds for x86-64/ARM64 including the Pi 5 vendor build/16 KiB pages; required exported symbols/providers verified; load/create/remove exercised; unsupported environment rejected |
| AY2 | Device, UDP binding, Netlink ownership, and lease machinery | Namespace-local same-port operation; stale owner/revision rejection; no forwarding before eligibility; controller crash closes new admission at lease expiry |
| AY3 | Record parser, AEAD, directional key generations, counters, replay | Independent vectors match; single counter reservation owner; post-auth concurrent replay recheck; wrong inputs do not advance trusted state; hard limits enforced |
| AY4 | Kernel activation/control/rekey transitions and bounded scheduling | Raj's exchange events integrate with kernel-owned transitions; loss/duplicates before and after commit; old TX cannot resume; current/retiring/pending state remains bounded |
| AY5 | Runtime accounting, teardown, kernel-path evidence | Queue/memory/allocation failure tests; pending crypto and late callbacks survive removal safely; no project leaks or lifetime diagnostics; ordinary data never relayed through userspace |

AY1 is a feasibility gate, not permission to replace the selected design silently. If a needed public API or hook/lifetime composition cannot satisfy the design, open a concrete design issue and update the specification with the affected owners before proceeding.

### 3.2 Raj

| ID | Work package | Required handoff evidence |
|---|---|---|
| RJ1 | Rust workspace, validated configuration, identities and CLI | Locked dependencies/toolchain; canonical bounded configuration; unknown/duplicate/overlapping fields rejected; secrets excluded from logs/status; sample configurations match the fixtures |
| RJ2 | Noise exchange, prologue/payload bindings, DH-result checks | Upstream and independent vectors; all-zero DH rejection; wrong pin/role/attempt/incarnation/profile/PMTU cases; exact cached handshake retransmission bytes |
| RJ3 | Netlink client and long-running controller | ABI round trips with AY2; one-time key handoff with correct Split direction; duplicate installation cannot reset counters; kernel alone protects all records; renewal/retry/shutdown tested in Linux |
| RJ4 | Transactional orchestration and recovery | Kernel configuration updates are transactional: affected peers blocked during multi-facility changes (nftables and routes are installed by `infra/gateway/vpt-gateway.sh`, not by the controller); invalid updates cannot broaden access; disruptive changes require fresh sessions |
| RJ5 | Control-plane adversarial/lifetime tests and diagnostics | Stale completions, responder limits, unilateral outage, cache/retry bounds, raw-key cleanup audit; portable tests also run on macOS but Linux integration remains mandatory |

### 3.3 Harsh

| ID | Work package | Required handoff evidence |
|---|---|---|
| HR1 | **Scrapped (3 Nov 2026):** the persistent nftables perimeter, packet marks and trusted packet context | Not required. The gateway uses one small forwarding ruleset (LAN to `vpn0` for the peers' prefixes, nothing else) installed by `infra/gateway/vpt-gateway.sh`. Merged HR1 code is listed for removal in [PERIMETER_REMOVAL.md](docs/PERIMETER_REMOVAL.md) |
| HR2 | ACL snapshots, TCP witnesses, UDP/echo associations, conntrack zones | Full allow/deny matrix; strict initiation; original source retained; current policy overrides stale witnesses; selective B2 removal leaves B flows eligible; fresh session uses clean zone |
| HR3 | Fragment normalization | Reordering, overlaps, duplicates, limits, ECN merge, peer/incarnation separation, expiry, and concurrent teardown; complete transport inspection before authorization |
| HR4 | MTU/DF, ICMP, checksum, TTL, DSCP/ECN and offload boundaries | 72-byte expansion; MTU ±1; DF/fragment behavior; exactly two forwarding hops; associated initial-flow errors; no unsupported outer fragments or invalid checksums admitted |
| HR5 | Policy-resource and concurrency qualification | RCU publication/final admission cut; concurrent revoke/rekey/flow creation; retired snapshot and zone exhaustion; all ownership/locking obligations documented with AY5 |

### 3.4 Anvit

| ID | Work package | Required handoff evidence |
|---|---|---|
| AN1 | Build/test environments and manifest | A reproducible, accessible Linux 6.18.y x86-64 reference VM with headers/ip/nft/conntrack; ARM64 build/runtime and Pi 5 16 KiB lanes; compiler/config/crypto requirements; Raj's Linux guest reproduces the shared smoke fixture |
| AN2 | Namespace runner and independent application/packet oracle | Core, multi-host, multi-peer, and capacity fixtures; root-namespace isolation checks; bounded cleanup; fresh HTTP IDs, healthy negative controls, synchronized captures and machine-readable results |
| AN3 | CI and test-tier orchestration | Static/unit builds, fresh disposable-VM integration, debug-kernel jobs, timeouts and artifact collection; failed/skipped required suites block the corresponding claim |
| AN4 | Adversarial and repeatability campaigns | Seeded impairment, independent parser/injector, boundary/failure/race cases with owners; mutation checks detect deliberately broken isolation/replay/length rules; A01–A43 coverage ledger |
| AN5 | Measurements and Pi demonstration | Separate benchmark fixture; both Pi manifests, power and radio qualification; all failures retained; manifest and reproducible evidence; Wi-Fi LAN (AP) and cable qualification; disabled/enabled/disabled physical test from frozen commit |

Do not leave AN4 and AN5 until the end. The fixture, healthy negative controls, and capture format are the first useful work; a small independent reference sender/receiver can exercise contracts before the real VPN is complete. Such an oracle is test-only and does not become a substitute userspace production data path.

## 4. Shared interfaces before parallel component work

| Contract | Owners who must agree | What is fixed before dependent work merges |
|---|---|---|
| VPT1 bytes and test vectors | Raj + Aayush; Anvit checks independently | Existing technical field offsets, lengths, AAD, nonce, Split direction, malformed-input rules; explicit vectors, no native-struct serialization |
| Netlink ABI v1 | Aayush + Raj | Commands/attributes from Technical Section 17; lengths/types/endianness; ownership/revision/attempt rules; exact error/result semantics; tested request/reply samples |
| Kernel object lifetime and locks | Aayush + Harsh | Peer/generation/context/witness reference ownership; allowed call contexts; lock order; consuming versus borrowing skb contracts; final admission/revocation linearization point |
| Policy and route configuration | Harsh + Raj | Exact allow-union/default-deny schema; stable rule IDs; zone ownership; safe transaction sequence and recovery; management exceptions separate from forwarding |
| Harness and observations | Anvit + all component owners | Test IDs, machine-readable result fields, counters/reset epochs, readiness events, trace association, failure cleanup; no hidden test bypasses |

One contract definition controls each interface. A machine-readable schema and generated constants/vectors may reduce duplication; generators themselves need review and checked-in golden byte examples. The independent test oracle must not derive all expectations through the implementation's parser or state machine.

The controller owns Noise handshake computation, immutable identity/configuration, exchange retries and orchestration. The kernel owns runtime eligibility, locators/attempt allocation, protected control execution, keys after handoff, counters/replay, witness state, admission and deadlines. Raj must not implement a competing userspace transport/replay state machine. Aayush and Harsh use one lifetime/accounting/admission framework; policy code must not retain raw pointers into retired configuration or invent a second peer object.

## 5. Repository boundaries

This is the intended source layout; these directories are not implemented by this plan.

```text
kernel/                 Aayush owns runtime/, control/, crypto/ and module build
  policy/               Harsh owns ACL/witness/conntrack integration
  ip/                   Harsh owns normalization/MTU/ICMP/ECN
  include/              Shared internal contracts: Aayush + Harsh
control/                Raj owns Rust controller/CLI/configuration
protocol/               Raj + Aayush own canonical wire/ABI descriptions and vectors
tests/
  unit/                 Component owners own focused tests
  conformance/          Raj + Aayush; Anvit owns independent oracle integration
  netns/                Anvit owns fixtures, runner and acceptance ledger
  adversarial/          All owners supply cases; Anvit integrates campaigns
  benchmark/            Anvit owns reproducible workload/measurement runner
  physical/             Anvit + Harsh own final Pi checks and runbook
tools/                  Anvit owns environment/build/test orchestration
docs/                   Contract notes, design changes, evidence index
.github/                Anvit owns CI/templates; all four review permissions
```

Root design/plan documents remain accessible here. Test fixtures use generated ephemeral identities, except explicitly public deterministic conformance keys. Never commit live private keys, build outputs, packet dumps containing sensitive real traffic, VM disks, or crash dumps. Put compact sanitized summaries/vector fixtures in Git; use bounded CI artifacts for bulk evidence, with hashes and retention recorded. Ownership is a review responsibility, not a ban on helping another person.

## 6. Linux and macOS development

| Developer | Editing/build workflow | Runtime qualification |
|---|---|---|
| Aayush, Harsh, Anvit | Linux source checkout; compile userspace normally; use a disposable test VM for privileged module/fault campaigns | Shared reference Linux 6.18.y image/config; exact matching module headers/build; x86-64 baseline and assigned ARM64 checks |
| Raj, Intel Mac | Native editing and portable Rust/vector tests; x86-64 Linux VM for Netlink, namespace, nftables and module integration | Same x86-64 reference image/config as the Linux developers |
| Raj, Apple Silicon Mac | Native editing and portable Rust/vector tests; ARM64 Linux VM for integration | Equivalent Linux 6.18.y ARM64 config/runtime, plus shared x86-64 CI evidence for cross-architecture changes |

Do not assume Raj's Mac CPU type. AN1 records it and supplies the appropriate Linux guest. The selected guest kernel must be bootable and rebuildable with matching headers, modules, debug configs and required facilities; a random distro's default kernel is not automatically the reference environment. Apple Silicon can use native ARM64 virtualization; x86-64 emulation is optional, slower functional evidence and never a comparable performance baseline.

Native macOS cannot load this Linux module or provide the Linux namespace/Netfilter path. A container on macOS runs against its underlying Linux VM kernel; it does not independently provide the project's custom-kernel contract. Use a full controllable Linux guest for integration. Containers may package unprivileged tools, not replace that qualification. A macOS-only green Rust job never establishes kernel or end-to-end acceptance.

The normal namespace VM fixture has at least two logical CPUs and 4 GiB RAM. Debug/fuzz builds need their own measured larger memory allocation and are not benchmarked against the normal fixture. Prefer SSH/remote editing inside a Linux filesystem rather than host-shared build directories with different permissions/case behavior. Management connectivity belongs only to the guest's root namespace; fixture namespaces have no link/route into it.

Record exact kernel patch/config/image and tool versions/hashes in the environment manifest once built. Pin Rust plus dependencies compatible with snow 0.10.0, compiler/binutils/LLVM, Python test dependencies, iproute2/nftables/conntrack-tools and packet tools. The Linux 6.18.y family is the design baseline, not permission for silent patch or toolchain drift. The inspected Pi candidate is 6.18.34+rpt-rpi-2712 with 16 KiB pages; it does not pin a generic VM image or qualify Pi A. Anvit publishes image/access location, hashes, matching headers, rebuild instructions and a clean network smoke result before AN1 is considered handed off. Update the manifest and rerun affected gates for changes. Do not suspend, restore, or clone live sessions; supported recovery must invalidate eligibility first as the design requires.

### 6.1 Platform migration handoffs (31 October)

[PLATFORM_PI5.md](docs/PLATFORM_PI5.md) records the observed Pi, required setup and qualification matrix. Anvit remains AN1/AN5 owner; the attached Pi is not a disposable VM. Aayush updates AY1/AY3 build gates, provider discovery and API adapters; Harsh reruns hook and zone checks on 6.18 and audits allocation/fragment/offload behavior on 16 KiB pages; Raj updates his interim guest and deployment checks while retaining the portable Noise/configuration tests. All owners refresh affected evidence rather than relabeling 6.12 results. Existing debug configs remain historical seeds until regenerated and run against 6.18.

The platform contract also records the migration status/exit criteria and kernel-update, boot-verification and recovery procedure. Anvit owns image/firmware recovery manifests; Aayush owns rebuild/API checks; Raj owns affected controller startup checks. A target change is not complete merely because the plan or Kbuild version string changed.

Power history must be resolved before stress/performance qualification. In the cable topology each Pi's radio hosts its laptop network, so a Pi's campus Wi-Fi connection ends while it does (manage the Pi over its own Wi-Fi network or the cable); hostapd and dnsmasq (`vpt-ap.sh`) are required on both Pis. Git, Rust, Clang and DKMS on the Pi are optional when deploying externally built artifacts. Ethernet speed remediation is deferred; record negotiated speed in evidence and make no faster-link claim.

## 7. Integration sequence and entry/exit gates

| Gate | Primary driver | Parallel work allowed | Exit evidence |
|---|---|---|---|
| G0: Contracts and environment | All; Anvit records | AY1/AN1, RJ1/RJ2 vectors, HR2 contract review, AN2 fixture | Required APIs/providers/builds feasible; namespace isolation passes with no VPN; byte/ABI/lifetime contracts reviewed; layouts and test IDs agreed |
| G1: Component qualification | Each owner | Runtime/control/policy/oracle implementation against frozen contracts | Focused unit/KUnit/conformance checks; malformed inputs and cleanup paths; cross-architecture builds; no unresolved critical API uncertainty |
| G2: First protected private-service path | Aayush + Raj + Harsh; Anvit runs | Continue boundary tests separately | Core fixture disabled/enabled/disabled, original source and reverse path proved, denied controls healthy; actual kernel path; no temporary blanket allow or plaintext fallback |
| G3: Full namespace behavior | Harsh + Anvit; all fix failures | Multi-host and M tests; loss/rekey/deadline/resources | All applicable A01–A43 and technical conformance families pass; independent-peer isolation; no missing required cases disguised as skips |
| G4: Release candidate qualification | All; Anvit assembles | Debug/race/fuzz campaigns and clean performance runs in separate VMs | Frozen commit; sanitizer/static checks, reproducibility, scale, repeated lifecycle tests, full evidence ledger; another developer reproduces clean setup |
| G5: Physical demonstration | Anvit + Harsh; Aayush/Raj support | Qualify actual hardware before demo; repeat the physical path tests | Exact same protocol/commit; ARM64 modules; Wi-Fi AP and cable evidence; honest separation of namespace M and two-Pi evidence |

G2 establishes a useful integrated result early. Kernel/security/multi-peer ambitions must not hide failure of the central private-service objective. A separately labeled connectivity illustration is allowed by the overall design, but the production profile never automatically falls back to unprotected operation.

No calendar deadlines are invented without the submission date and availability. Work starts at G0; package issues show owner, contract dependencies, acceptance cases, reviewer, next deliverable and actual evidence. Bring integration failures into small shared fixes promptly; do not develop four isolated components until the final week.

The first concrete handoffs are: Aayush produces the minimal public-API/module lifetime feasibility result and shared kernel object contract; Raj produces validated sample configuration and independent exchange/Netlink vectors; Harsh produces the exact route/guard/zone and admission contract; Anvit produces N0 with healthy listeners and proven disabled isolation, before it needs a working VPN. Review these together, then open dependent component PRs. Do not wait for the tunnel to exist before discovering that a fixture or shared interface is wrong.

## 8. GitHub collaboration contract

Use the existing VPNTunnel repository, not the historical project. Start short-lived branches from current `main`, such as `codex/aayush/netns-runtime`, `codex/raj/noise-controller`, `codex/harsh/flow-witness`, and `codex/anvit/netns-fixture`. One coherent issue per PR where practical; draft PRs expose contracts early. Avoid rewriting other people's branches or mixing unrelated formatting with functional changes.

Aayush sets `main` protection/rules when the repository's permissions/plan support it: PRs, required relevant checks, current review after meaningful changes, conversation resolution, and no force pushes/deletion. Ordinary PRs need at least one non-author reviewer. Security/wire/ABI/ownership/locking/policy changes need two non-author reviewers covering both affected domains. CODEOWNERS should be populated with each person's verified GitHub handle; names are not guessed. These settings are requested workflow, not changes already applied to GitHub. [GitHub branch-protection reference](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches)

Each PR states: behavior changed, design sections/invariants affected, interface/layout/lifetime changes, tests and exact environment, results/artifact location, and remaining limitations. Affected owners review changes to shared contracts even if code lives in another directory. A protocol or limit change updates the technical specification and vectors in the same review; peers never negotiate an undocumented compatibility mode.

Before merge, required jobs evaluate the current PR head against current integration state. A stale green result does not cover a new merge conflict resolution. A failed or missing required privileged job is not replaced by a local statement that it probably works. Anvit creates an aggregate gate that fails when any required child job fails, is cancelled, or is unexpectedly skipped; documentation-only exceptions must be explicit and still run document validation. The initial [portable CI lane](docs/PORTABLE_CI.md) covers only its named Python, shell and Rust checks; the privileged VM and architecture lanes are still missing and are not implied by that gate.

Public/untrusted PR code never gets secrets or arbitrary privileged access to a persistent team machine. Use disposable VM jobs for kernel execution; rebuild/destroy guests between jobs. Pin external Actions by full commit SHA, minimize token permissions, and do not check out untrusted code through a privileged `pull_request_target` workflow. The host remains trusted and must not accept guest-controlled paths/commands without validation. [GitHub Actions secure-use guidance](https://docs.github.com/en/actions/reference/security/secure-use)

No automatic account messaging, GitHub issue creation, protection change, push, or merge is performed by this planning update. The deliverable here is the reviewable team contract.

## 9. Definition of done and final speaking roles

A package is done only when its functionality and failure paths match the design, focused tests pass, the non-author review is resolved, affected integration checks pass, and evidence/diagnostics are available. A feature is qualified only on the recorded architecture/kernel/config and workload. Do not mark tests complete because their scripts exist.

| Person | Final explanation and evidence responsibility |
|---|---|
| Aayush | Trace packet/device/socket/crypto processing; prove kernel data path and safe runtime/teardown |
| Raj | Explain identity trust, Noise exchange, key handoff, activation/rekey and controller failure |
| Harsh | Explain routes, source preservation, strict initiation, policy isolation, MTU/fragment/error correctness |
| Anvit | Run reproducible disabled/enabled/disabled test, show independent observations, coverage and measurements |

All four review the release ledger. No unresolved unauthorized forwarding, key/nonce reuse, replay delivery, lifetime/concurrency fault, unbounded allocation, or required unsupported environment can be hidden as a cosmetic issue. Lesser limits must be stated and approved as a scope change with the affected acceptance claim updated; passing a classroom demo does not certify a production VPN.
