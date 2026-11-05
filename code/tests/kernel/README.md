# Kernel configuration seeds

These `.config` fragments seed the kernel builds used by the kernel unit tests. They
target a clean Linux 6.18 x86-64 source tree (one ARM64 layout fragment aside) and enable
KUnit plus the sanitizers each suite needs. They are build inputs, not full networking
reference configs: the reference VM supplies the complete-network image that the
namespace tests run against.

How to use a seed:

1. Copy it into an empty output tree for the pinned 6.18 source.
2. Run `make olddefconfig` with the intended compiler (GCC or Clang).
3. Review the resolved config — SMP, KUnit, the crypto providers, module
   load/unload, and the diagnostics the seed selects (KASAN, KCSAN, UBSAN, lockdep).
4. Build and run the applicable suite (see `docs/testing.md`).

The seeds by diagnostic profile:

- `ay3-debug-x86_64.config` — GCC KASAN/UBSAN/lockdep debug build.
- `ay3-kcsan-x86_64.config` — GCC KCSAN data-race build.
- `ay3-clang-x86_64.config` — Clang KASAN build.
- `hr2-acl-debug.config` — KUnit/KASAN fragment for the isolated ACL helper.
- `hr3-fragment-debug.config` — debug build for the fragment / ICMP / MTU adapter.
- `hr3-fragment-arm64-layout.config` — ARM64 layout fragment for the same adapter.
- `hr5-policy-debug.config`, `hr5-policy-kcsan.config` — debug and KCSAN builds for the
  policy and reload path.

The Pi 5 vendor build uses 16 KiB pages and its own matching configuration; do not reuse
the x86 configs or a generic ARM64 VM module on it (see `docs/pi-setup.md`).
