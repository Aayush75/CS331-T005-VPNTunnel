# Reproducing the results

Everything that loads the kernel module or creates network namespaces needs root on a
**Linux 6.18.y** machine, and the module/suite load and unload kernel modules globally,
so it must be a **disposable** machine. We provide a reference VM that is exactly that.
Nothing here needs any hardware; the two-Pi hardware demo is separate
([pi-setup.md](pi-setup.md)).

## 1. The reference VM

A ready-to-run Linux 6.18 x86-64 VM image (QEMU qcow2) with the kernel headers,
toolchains, and network tools already installed.

- **Download:** <https://drive.google.com/file/d/16ghaL8cqNT5avPYj0CVNKsPrq5PGa0l3/view?usp=sharing>
- **Login:** user `testuser`, password `password` (it can use `sudo`).

The image is a long-lived writable working copy — run it directly, or take your own
overlay/snapshot first if you want to reset later.

### Boot it with QEMU

```bash
qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 2 -m 4096 \
  -drive file=vpt1-ref-6.18-x86_64.qcow2,if=virtio \
  -nic user,hostfwd=tcp::2222-:22 \
  -nographic
```

(On Windows use `-accel whpx` instead of `-enable-kvm`; on macOS/Apple Silicon the
image is x86-64, so expect emulation speed.) Then SSH in:

```bash
ssh -p 2222 testuser@127.0.0.1      # password: password
```

Confirm the kernel:

```bash
uname -r        # must be 6.18.y
```

## 2. Get the code onto the VM

Either clone the repository inside the VM, or copy this `code/` directory in with
`scp -P 2222 -r code testuser@127.0.0.1:~/`. The rest of this guide runs inside the VM,
from the `code/` directory.

## 3. Build

```bash
cd code
./scripts/build.sh
```

This builds the kernel module in both release and debug form (debug adds the in-kernel
unit-test modules) and the `vptctl` controller. The build **refuses** any kernel that is
not 6.18.y — that is expected on any other machine; use the VM.

## 4. Run the full test suite

```bash
sudo ./scripts/test.sh
```

This loads each kernel unit-test module, runs every end-to-end network-namespace test
(data path, re-key, liveness, activation, fragments/ICMP/MTU, multi-peer isolation,
configuration reload, runtime accounting, and the demo reachability test), runs the
controller's Rust tests, and scans for leaks at unload. It prints `ALL TESTS PASSED`
on success. Expect it to take several minutes.

To run a single test instead, see [testing.md](testing.md).

## 5. Run the demonstration

```bash
sudo ./scripts/demo-netns.sh
```

This builds the five-namespace topology (client, gateway A, underlay, gateway B,
service), brings the VPN up with the real controller and kernel module, and checks that
the service is **unreachable with the VPN off and reachable with it on**, in both
directions, then fails closed when a gateway stops. This is the core result the project
is about, with no hardware.

## 6. Benchmarks

```bash
sudo ./scripts/bench-netns.sh         # tunnel vs a direct no-tunnel baseline
sudo ./scripts/bench-packetsize.sh    # goodput vs datagram size
```

Results land under `results/<timestamp>-*/summary.md`. See
[benchmarking.md](benchmarking.md) for what they measure. These need `iperf3`
(preinstalled on the VM).

## 7. ARM64 / Raspberry Pi build check (optional, no hardware)

If you have an aarch64 cross toolchain and a prepared ARM64 6.18 kernel tree, you can
cross-build the module and run the 16 KiB-page audit without a Pi:

```bash
KDIR=/path/to/arm64-6.18-build CROSS_COMPILE=aarch64-linux-gnu- ./scripts/arm64-build.sh
```

This is a build + inventory check only; the Pi runtime is in [pi-setup.md](pi-setup.md).

## Troubleshooting

| Symptom | What to check |
|---|---|
| `build refuses: requires 6.18.y` | You are not on the reference VM. Boot it (step 1). |
| `insmod: Invalid parameters` on a repeat run | A previous module is still loaded: `sudo rmmod vpt1_foundation`. |
| A test needs `vptctl` | `./scripts/build.sh` first, or let the script build it. |
| `Permission denied` writing test objects | A prior run left root-owned build files: `sudo make -C kernel clean`. |
| namespaces left over after a crash | They are named `vpt_*`; `sudo ip netns list` then `sudo ip netns del <name>`. |
| suite seems to hang | A single long test (`ay4_soak`) runs ~45 s; give it time, or run tests individually. |
