#!/usr/bin/env python3
"""Build a throwaway initramfs and run AY1 without host privileges.

Requires a trusted extracted kernel package tree (with depmod metadata), static
BusyBox, host iproute2, and already built matching project modules. No downloads.
"""
import argparse
from collections import Counter
import hashlib
import json
import gzip
import lzma
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, timeout=30, **kwargs)


def assess_output(output, returncode):
    starts = Counter(re.findall(r'vpt1_ay1: net_init ns=(\d+) incarnation=(\d+)', output))
    exits = Counter(re.findall(r'vpt1_ay1: net_exit ns=(\d+) incarnation=(\d+)', output))
    bad = re.search(r'BUG:|WARNING:|Oops:|Kernel panic|refcount_t:|AY1_SETUP_ERROR', output)
    return (returncode == 0 and 'AY1_SMOKE_PASS' in output and
            'AY1_GUEST_RESULT=0' in output and
            'AY1_PROVIDER_REJECTION_PASS' in output and
            bool(starts) and starts == exits and not bad)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sysroot', type=Path, required=True)
    p.add_argument('--arch', choices=('x86_64', 'arm64'), default='x86_64')
    p.add_argument('--guest-root', type=Path)
    p.add_argument('--qemu')
    p.add_argument('--cpu', help='override QEMU CPU model')
    p.add_argument('--memory-mib', type=int, default=4096)
    p.add_argument('--release', required=True)
    p.add_argument('--modules', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    args = p.parse_args()
    if args.memory_mib <= 0:
        p.error('--memory-mib must be positive')
    args.sysroot = args.sysroot.resolve()
    args.modules = args.modules.resolve()
    kernel = args.sysroot / 'boot' / ('vmlinuz-' + args.release)
    busybox = args.sysroot / 'usr/bin/busybox'
    repo = Path(__file__).resolve().parents[1]
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ay1-qemu-') as tmp:
        root = Path(tmp) / 'root'
        for name in ('bin', 'proc', 'sys', 'dev', 'run/netns', 'var', 'tmp', 'modules'):
            (root / name).mkdir(parents=True, exist_ok=True)
        (root / 'var/run').symlink_to('/run')
        guest_root = args.guest_root.resolve() if args.guest_root else None
        guest_busybox = guest_root / 'usr/bin/busybox' if guest_root else busybox
        shutil.copy2(guest_busybox, root / 'bin/busybox')
        applets = run([str(busybox), '--list'], capture_output=True, text=True).stdout.split()
        for applet in applets:
            if applet != 'busybox':
                (root / 'bin' / applet).symlink_to('/bin/busybox')
        # Guest binaries are trusted inputs, never downloaded during execution.
        (root / 'bin/ip').unlink(missing_ok=True)
        if guest_root:
            ip_candidates = [guest_root / 'usr/sbin/ip', guest_root / 'usr/bin/ip']
            ip = next(path for path in ip_candidates if path.exists())
            shutil.copy2(ip, root / 'bin/ip')
            for library in (guest_root / 'usr/lib').rglob('*'):
                if library.is_file() and ('.so' in library.name):
                    dst = root / library.relative_to(guest_root)
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(library, dst)
            (root / 'lib').symlink_to('/usr/lib')
        else:
            ip = Path(shutil.which('ip')).resolve()
            shutil.copy2(ip, root / 'bin/ip')
            libs = run(['ldd', str(ip)], capture_output=True, text=True).stdout
            for name in re.findall(r'(/[^\s()]+)', libs):
                src = Path(name)
                dst = root / src.relative_to('/')
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        dependencies = []
        for module in ('crypto-chacha20', 'chacha20poly1305',
                       'udp_tunnel', 'nf_conntrack', 'nf_tables'):
            output = run(['modprobe', '-C', '/dev/null', '--ignore-install', '--show-depends', '-d', str(args.sysroot),
                          '-S', args.release, module], capture_output=True, text=True).stdout
            for line in output.splitlines():
                if line.startswith('insmod '):
                    src = Path(line.split()[1])
                    if src not in dependencies:
                        dependencies.append(src)
        load = []
        for i, src in enumerate(dependencies):
            dst = root / 'modules' / (str(i) + '.ko')
            if src.suffix == '.xz':
                dst.write_bytes(lzma.decompress(src.read_bytes()))
            elif src.suffix == '.ko':
                shutil.copy2(src, dst)
            else:
                raise ValueError('unsupported module compression: ' + str(src))
            load.append('insmod /modules/' + dst.name + ' || { echo AY1_SETUP_ERROR; poweroff -f; }')
        for name in ('vpt1_foundation.ko', 'vpt1_api_probe.ko'):
            shutil.copy2(args.modules / name, root / 'modules' / name)
        shutil.copy2(repo / 'tests/netns/ay1_smoke.sh', root / 'smoke.sh')
        init = root / 'init'
        init.write_text('''#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
# The fresh guest deliberately has no loaded AEAD provider yet.
if insmod /modules/vpt1_foundation.ko; then
    echo AY1_SETUP_ERROR_unexpected_provider
    poweroff -f
fi
if [ -d /sys/module/vpt1_foundation ]; then
    echo AY1_SETUP_ERROR_partial_registration
    poweroff -f
fi
if ! dmesg | grep -q 'vpt1_ay1: required AEAD unavailable/incompatible: -2$'; then
    echo AY1_SETUP_ERROR_wrong_provider_failure
    poweroff -f
fi
if dmesg | grep -q 'vpt1_ay1: net_init'; then
    echo AY1_SETUP_ERROR_state_published_without_provider
    poweroff -f
fi
echo AY1_PROVIDER_REJECTION_PASS
''' + '\n'.join(load) + '''
AY1_IP=/bin/ip AY1_DISPOSABLE_VM=yes sh /smoke.sh /modules
rc=$?
echo AY1_GUEST_RESULT=$rc
dmesg
poweroff -f
''')
        init.chmod(0o755)
        archive = Path(tmp) / 'initramfs.gz'
        files = run(['find', '.', '-print0'], cwd=root, capture_output=True).stdout
        packed = run(['cpio', '--null', '-o', '--format=newc'], cwd=root,
                     input=files, capture_output=True).stdout
        archive.write_bytes(gzip.compress(packed))
        with args.log.open('w') as log:
            try:
                arch_args = (['-machine', 'virt', '-cpu', 'cortex-a72']
                             if args.arch == 'arm64' else ['-cpu', 'max'])
                if args.cpu:
                    arch_args[-1] = args.cpu
                console = 'ttyAMA0' if args.arch == 'arm64' else 'ttyS0'
                emulator = args.qemu or ('qemu-system-aarch64' if args.arch == 'arm64'
                                         else 'qemu-system-x86_64')
                result = subprocess.run([
                    emulator, '-accel', 'tcg', *arch_args, '-m', str(args.memory_mib), '-smp', '2',
                    '-nodefaults', '-no-reboot', '-nographic', '-serial', 'stdio',
                    '-nic', 'none', '-kernel', str(kernel), '-initrd', str(archive),
                    '-append', 'console=' + console + ' panic=-1 oops=panic'],
                    stdout=log, stderr=subprocess.STDOUT, timeout=180)
            except subprocess.TimeoutExpired:
                print('ERROR: QEMU timed out; see', args.log)
                return 1
        output = args.log.read_text()
        passed = assess_output(output, result.returncode)
        inputs = [kernel, busybox, guest_busybox, ip, args.modules / 'vpt1_foundation.ko',
                  args.modules / 'vpt1_api_probe.ko', repo / 'tests/netns/ay1_smoke.sh',
                  repo / 'kernel/runtime/foundation.c', repo / 'kernel/probes/api_probe.c',
                  repo / 'kernel/Makefile']
        summary = {'status': 'PASS' if passed else 'FAIL', 'arch': args.arch,
                   'release': args.release, 'scope': 'AY1 lifecycle smoke only',
                   'accelerator': 'TCG', 'cpus': 2, 'ram_mib': args.memory_mib,
                   'cpu_model': args.cpu or ('cortex-a72' if args.arch == 'arm64' else 'max'),
                   'network_devices': 0, 'log_sha256': hashlib.sha256(args.log.read_bytes()).hexdigest(),
                   'inputs': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in inputs},
                   'dependencies': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                    for path in dependencies}}
        args.log.with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n')
        print('PASS' if passed else 'FAIL', args.log)
        return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
