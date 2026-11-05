#!/usr/bin/env python3
"""Run a component test module in a fresh, offline, unprivileged QEMU guest."""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def assess(output, returncode, suite="parser", backend="standalone"):
    label = "VPT1_" + suite.upper()
    if (returncode != 0 or label + '_GUEST_PASS' not in output or
            'VPT1_GUEST_CPUS=0-1' not in output):
        return False
    if re.search(r'BUG:|WARNING:|Oops:|Kernel panic|refcount_t:|GUEST_ERROR|'
                 r'UBSAN:|KCSAN:|possible circular locking|possible recursive locking|'
                 r'inconsistent lock state', output):
        return False
    if backend == 'kunit':
        clean = re.sub(r'^\[[^]]+\]\s*', '', output, flags=re.MULTILINE)
        totals = re.search(r'# vpt1-' + suite +
                           r': pass:(\d+) fail:0 skip:0 total:(\d+)', clean)
        minimum = {'parser': 7, 'sequence': 5, 'aead': 2, 'acl': 6,
                   'flow': 5, 'zone': 4, 'fragment': 14, 'ip': 8, 'policy': 8}.get(suite)
        return bool(totals and minimum is not None and int(totals[1]) == int(totals[2]) and
                    int(totals[1]) >= minimum and '# Subtest: vpt1-' + suite in clean and
                    re.search(r'^\s*ok \d+(?: -)? vpt1-' + suite + r'(?:\s|$)',
                              clean, re.MULTILINE) and
                    not re.search(r'^\s*not ok|# SKIP', clean, re.MULTILINE))
    match = re.search(label + r'_TEST: assertions=(\d+) failures=0 PASS', output)
    minimum = 350 if suite == 'gateway' else 1001
    return bool(match and int(match[1]) >= minimum)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, required=True, help='static guest binary')
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--arch', choices=('x86_64', 'arm64'), default='x86_64')
    parser.add_argument('--qemu')
    parser.add_argument('--provider-root', type=Path)
    parser.add_argument('--release')
    parser.add_argument('--backend', choices=('standalone', 'kunit'), default='standalone')
    parser.add_argument('--builtin-aead', action='store_true')
    parser.add_argument('--suite', choices=('parser', 'sequence', 'aead', 'acl', 'flow', 'zone',
                                            'fragment', 'ip', 'policy', 'gateway'), default='parser')
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    if args.suite == 'gateway' and args.backend != 'standalone':
        parser.error('gateway module uses the standalone backend')
    for name in ('kernel', 'busybox', 'module'):
        setattr(args, name, getattr(args, name).resolve())
    args.log.parent.mkdir(parents=True, exist_ok=True)
    module_name = 'vpt1_gwtest' if args.suite == 'gateway' else 'vpt1_' + args.suite + '_test'
    label = 'VPT1_' + args.suite.upper()
    summary = dict(status='ERROR', suite=args.suite, backend=args.backend, architecture=args.arch, runtime='TCG',
                   cpus=2, ram_mib=4096, network_devices=0)
    try:
        with tempfile.TemporaryDirectory(prefix='ay3-qemu-') as temp:
            root = Path(temp) / 'root'
            for name in ('bin', 'dev', 'proc', 'sys'):
                (root / name).mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.busybox, root / 'bin/busybox')
            for name in ('sh', 'mount', 'insmod', 'rmmod', 'dmesg', 'poweroff', 'uname', 'cat'):
                (root / 'bin' / name).symlink_to('/bin/busybox')
            shutil.copy2(args.module, root / (module_name + '.ko'))
            provider_files = []
            load_commands = []
            if args.suite in ('aead', 'policy', 'gateway') and not args.builtin_aead:
                if not args.provider_root or not args.release:
                    raise ValueError('crypto suites require --provider-root and --release')
                for module in ('crypto-chacha20', 'chacha20poly1305'):
                    dependencies = subprocess.run(
                        ['modprobe', '-C', '/dev/null', '--ignore-install', '--show-depends',
                         '-d', str(args.provider_root), '-S', args.release, module],
                        check=True, capture_output=True, text=True, timeout=10).stdout
                    for line in dependencies.splitlines():
                        if not line.startswith('insmod '):
                            continue
                        source = Path(line.split()[1])
                        if source in provider_files:
                            continue
                        provider_files.append(source)
                        name = 'provider-' + str(len(provider_files)) + '.ko'
                        data = source.read_bytes()
                        if source.suffix == '.xz':
                            data = lzma.decompress(data)
                        elif source.suffix != '.ko':
                            raise ValueError('unsupported provider compression')
                        (root / name).write_bytes(data)
                        load_commands.append('insmod /' + name +
                                             ' || { echo GUEST_ERROR; poweroff -f; }')
            init = root / 'init'
            init.write_text('''#!/bin/sh
export PATH=/bin
mount -t proc proc /proc || { echo GUEST_ERROR; poweroff -f; }
mount -t sysfs sysfs /sys || { echo GUEST_ERROR; poweroff -f; }
mount -t devtmpfs devtmpfs /dev || { echo GUEST_ERROR; poweroff -f; }
uname -a
online=$(cat /sys/devices/system/cpu/online)
echo VPT1_GUEST_CPUS=$online
[ "$online" = 0-1 ] || { echo GUEST_ERROR_cpu_count; poweroff -f; }
#PROVIDERS
if insmod /vpt1_parser_test.ko && rmmod vpt1_parser_test; then
    echo VPT1_PARSER_GUEST_PASS
else
    echo GUEST_ERROR
fi
dmesg
poweroff -f
'''.replace('#PROVIDERS', '\n'.join(load_commands)).replace('vpt1_parser_test', module_name).replace('VPT1_PARSER', label))
            init.chmod(0o755)
            files = subprocess.run(['find', '.', '-print0'], cwd=root, check=True,
                                   capture_output=True, timeout=10).stdout
            packed = subprocess.run(['cpio', '--null', '-o', '--format=newc'], cwd=root,
                                    input=files, capture_output=True, timeout=30,
                                    check=True).stdout
            archive = Path(temp) / 'initramfs.gz'
            archive.write_bytes(gzip.compress(packed))
            arm = args.arch == 'arm64'
            qemu = args.qemu or ('qemu-system-aarch64' if arm else 'qemu-system-x86_64')
            machine = ['-machine', 'virt', '-cpu', 'cortex-a72'] if arm else ['-cpu', 'max']
            command = [qemu, '-accel', 'tcg', *machine, '-m', '4096', '-smp', '2',
                       '-nodefaults', '-no-reboot', '-nographic', '-serial', 'stdio',
                       '-nic', 'none', '-kernel', str(args.kernel), '-initrd', str(archive),
                       '-append', 'console=' + ('ttyAMA0' if arm else 'ttyS0') +
                       ' panic=-1 oops=panic kunit.enable=1']
            with args.log.open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=120)
            output = args.log.read_text()
            summary['status'] = 'PASS' if assess(output, result.returncode, args.suite, args.backend) else 'FAIL'
            assertions = re.search(label + r'_TEST: assertions=(\d+)', output)
            summary['assertions'] = int(assertions[1]) if assertions else None
            totals = re.search(r'# vpt1-' + args.suite +
                               r': pass:(\d+) fail:0 skip:0 total:(\d+)', output)
            summary['cases'] = int(totals[2]) if totals else None
            cpus = re.search(r'VPT1_GUEST_CPUS=([^\r\n]+)', output)
            summary['online_cpus'] = cpus[1] if cpus else None
            summary['log_sha256'] = digest(args.log)
            inputs = [args.kernel, args.busybox, args.module,
                      ROOT / ('tests/unit/vpt1_' + args.suite + '_test.c')]
            if args.suite in ('parser', 'sequence', 'acl', 'flow', 'zone'):
                header = {'parser': 'vpt1_wire.h', 'sequence': 'vpt1_sequence.h',
                          'acl': 'vpt1_acl.h', 'flow': 'vpt1_flow_state.h',
                          'zone': 'vpt1_zone_pool.h'}[args.suite]
                directory = 'policy' if args.suite in ('acl', 'flow', 'zone') else 'crypto'
                source = {'flow': 'flow_state', 'zone': 'zone_pool'}.get(args.suite, args.suite)
                inputs.extend([ROOT / ('kernel/' + directory + '/' + source + '.c'),
                               ROOT / 'kernel/include' / header])
            if args.suite == 'parser':
                inputs.append(ROOT / 'tests/conformance/vpt1_parser_vectors.json')
            if args.suite == 'ip':
                inputs.extend([ROOT / 'kernel/ip/ipv4.c', ROOT / 'kernel/include/vpt1_ip.h',
                               ROOT / 'kernel/ip/error.c', ROOT / 'kernel/include/vpt1_error.h',
                               ROOT / 'kernel/include/vpt1_gateway.h'])
            if args.suite == 'fragment':
                inputs.extend([ROOT / 'kernel/ip/fragment.c',
                               ROOT / 'kernel/include/vpt1_fragment.h'])
            if args.suite in ('policy', 'gateway'):
                inputs.extend(ROOT / path for path in (
                    'kernel/runtime/gateway.c', 'kernel/include/vpt1_gateway.h',
                    'kernel/ip/ipv4.c', 'kernel/include/vpt1_ip.h',
                    'kernel/include/vpt1_error.h',
                    'kernel/policy/acl.c', 'kernel/include/vpt1_acl.h',
                    'kernel/crypto/generation.c', 'kernel/include/vpt1_generation.h',
                    'kernel/crypto/sequence.c', 'kernel/include/vpt1_sequence.h',
                    'kernel/crypto/parser.c', 'kernel/include/vpt1_wire.h'))
            if args.suite == 'policy':
                inputs.extend(ROOT / path for path in (
                    'kernel/policy/flow_state.c', 'kernel/include/vpt1_flow_state.h',
                    'kernel/policy/zone_pool.c', 'kernel/include/vpt1_zone_pool.h'))
            if args.suite == 'gateway':
                inputs.extend([ROOT / 'kernel/ip/fragment.c',
                               ROOT / 'kernel/include/vpt1_fragment.h'])
            if args.suite == 'aead':
                inputs.append(ROOT / 'tests/conformance/vpt1_aead_vectors.json')
                inputs.extend(provider_files)
            summary['inputs'] = {str(path): digest(path) for path in inputs}
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        summary['reason'] = str(exc)
    args.log.with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(summary['status'], args.log)
    return 0 if summary['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
