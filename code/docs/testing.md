# Testing

`sudo ./scripts/test.sh` runs everything below in one pass (debug and release builds of
the module). This document says what each part covers and how to run one test on its own.
All of it runs on the reference VM; see [reproducing.md](reproducing.md).

## Layers

| Layer | Where | What it checks |
|---|---|---|
| Kernel unit modules | `tests/unit/*.c`, built as `.ko` | Each pure component in isolation: the record parser, sequence/replay, AEAD vectors, the ACL, flow state, the zone pool, key generations, fragment reassembly, IP validation, the gateway state machine, the policy path, and the flow-witness table. Each module prints `... PASS` with an assertion count. |
| End-to-end namespace tests | `tests/netns/*.py` | The whole data path across real namespaces with the real module and controller: Netlink provisioning, the two-gateway data path, re-key without loss, liveness and closure, the activation handshake, the live fragment / ICMP / MTU adapter, multi-peer isolation, configuration reload and validation, runtime accounting, and the before/after demonstration. |
| Controller tests | `control/tests/*.rs` | The handshake against an independent Noise vector and a from-scratch VPTunnel INIT/RESPONSE vector, the Netlink message layer, configuration validation, and adversarial control-plane cases. Portable (macOS/Linux); the Netlink-against-a-real-kernel test needs Linux. |
| Build quality | `tools/` | `W=1` warning-free builds, Sparse, Clang analysis, and (where the kernel supports them) KASAN / UBSAN / lockdep / kmemleak and KCSAN fragments in `tools/kasan.frag` and `tools/kcsan.frag`. |

## Running one test

The suite harness is `tools/kernel_vm_suite.sh`; it builds, loads each module, runs the
namespace tests, and unloads. To run a **single** namespace test instead, build first,
load the foundation module, and invoke it directly (as root, on the VM):

```bash
cd code
make -C kernel W=1 VPT1_DEBUG=1
sudo insmod kernel/vpt1_foundation.ko
VPTCTL=control/target/release/vptctl sudo -E python3 tests/netns/demo_reachability.py "$VPTCTL"
sudo rmmod vpt1_foundation
```

A single **kernel unit module** is loaded on its own; its result is in the kernel log:

```bash
sudo insmod kernel/vpt1_witness_test.ko ; dmesg | tail -1 ; sudo rmmod vpt1_witness_test
```

The controller tests run without the kernel:

```bash
cd control && cargo test --release --locked
```

## What a pass means

- Every unit module prints `PASS` with zero failures.
- Every namespace test prints `<NAME> PASS passed=N failed=0`.
- The module unloads with no leaked objects (`gw=0 peer=0 cfg=0 gen=0`) and no
  `BUG`/`WARNING`/lockdep/KASAN line in the kernel log.
- The suite ends `KERNEL_VM_SUITE PASS` and `./scripts/test.sh` prints `ALL TESTS PASSED`.

## Known environment notes

- The suite loads and unloads kernel modules globally — run it only on the disposable VM.
- A `kmemleak` scan runs only if the booted kernel has kmemleak; otherwise that step is
  reported as skipped, not failed.
- ARM64 / Raspberry Pi runtime is a separate lane ([pi-setup.md](pi-setup.md)); the
  cross-build check is `./scripts/arm64-build.sh`.
