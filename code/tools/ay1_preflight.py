#!/usr/bin/env python3
"""Read-only AY1 build-environment inventory; never establishes runtime PASS."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess

CONFIG = (
    'MODULES', 'NET', 'INET', 'NET_NS', 'NET_UDP_TUNNEL', 'NETFILTER',
    'NF_CONNTRACK', 'NF_CONNTRACK_ZONES', 'NF_TABLES', 'VETH', 'BRIDGE',
    'CRYPTO_AEAD', 'CRYPTO_CHACHA20POLY1305',
)
SYMBOLS = (
    'register_pernet_subsys', 'unregister_pernet_subsys',
    'rtnl_link_register', 'rtnl_link_unregister', 'register_netdevice',
    'unregister_netdevice_queue', 'udp_sock_create4', 'sock_create_kern',
    'ip_sock_set_freebind', 'kernel_bind', 'setup_udp_tunnel_sock',
    'udp_tunnel_sock_release', 'genl_register_family', 'genl_unregister_family',
    'nf_register_net_hooks', 'nf_unregister_net_hooks',
    'nf_conntrack_find_get', 'nf_ct_get_tuplepr', 'nf_ct_tmpl_alloc',
    'crypto_alloc_aead', 'crypto_aead_setauthsize',
)


def parse_config(text):
    return dict(re.findall(r'^CONFIG_(\w+)=(\S+)$', text, re.MULTILINE))


def parse_symbols(text):
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 4:
            result[fields[1]] = {'crc': fields[0], 'module': fields[2],
                                 'export': fields[3]}
    return result


def assess(release, config, symbols):
    checks = [{'name': 'kernel_family', 'status': 'PASS' if
               re.fullmatch(r'6\.18\.\d+[-+.\w]*', release) else 'FAIL',
               'value': release}]
    for name in CONFIG:
        value = config.get(name, 'missing')
        checks.append({'name': 'CONFIG_' + name, 'value': value,
                       'status': 'PASS' if value in ('y', 'm') else 'FAIL'})
    for name in SYMBOLS:
        checks.append({'name': name, 'value': symbols.get(name),
                       'status': 'PASS' if name in symbols else 'FAIL'})
    return checks


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kdir', required=True, type=Path)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        release_file = args.kdir / 'include/config/kernel.release'
        symbol_file = args.kdir / 'Module.symvers'
        release = release_file.read_text().strip()
        checks = assess(release, parse_config(args.config.read_text()),
                        parse_symbols(symbol_file.read_text()))
        git = subprocess.run(['git', 'rev-parse', 'HEAD'], check=True,
                             capture_output=True, text=True, timeout=10).stdout.strip()
        dirty = subprocess.run(['git', 'status', '--porcelain'], check=True,
                               capture_output=True, text=True, timeout=10).stdout
        result = {'status': 'PASS' if all(c['status'] == 'PASS' for c in checks)
                  else 'FAIL', 'scope': 'build prerequisites only',
                  'runtime': 'NOT_RUN', 'host_release': platform.release(),
                  'host_arch': platform.machine(), 'git_commit': git,
                  'git_dirty': bool(dirty), 'checks': checks,
                  'inputs': {str(p): digest(p) for p in
                             (args.config, symbol_file, release_file)}}
    except (OSError, subprocess.SubprocessError) as exc:
        result = {'status': 'ERROR', 'reason': str(exc), 'runtime': 'NOT_RUN'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(result['status'], args.output)
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
