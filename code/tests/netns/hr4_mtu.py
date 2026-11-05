#!/usr/bin/env python3
"""Configured P=1280 on a real 1280-byte carrier, including Linux's DF-set PTB.

Root, foundation loaded, disposable VM only. This does not qualify automatic
PMTU discovery, associated tunnel ICMP records, or DF-clear normalization.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time

import ay2_datapath as D
from hr4_ip import root_network

SRC, DST = '10.77.1.10', '10.77.2.10'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or os.environ.get('VPT1_DISPOSABLE_VM') != 'yes':
        parser.error('requires root and VPT1_DISPOSABLE_VM=yes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    directory = args.output.parent / (args.output.stem + '-packets')
    directory.mkdir(exist_ok=True)
    report = dict(status='ERROR', cases=[], pmtu=1280, tx_mtu=1208, underlay_mtu=1280,
                  scope='configured static bridge path and local DF-set PTB only')
    gateways, sockets = [], []
    before = root_network()

    def check(name, condition):
        report['cases'].append(dict(name=name, passed=bool(condition)))
        if not condition:
            print(f'FAIL {name}', flush=True)

    try:
        result = subprocess.run(['bash', str(Path(D.HERE) / 'n1_fixture.sh'), 'setup', D.RUN],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        for site, interfaces in (('gwa', ('veth_gwa_out',)), ('gwb', ('veth_gwb_out',)),
                                 ('und', ('veth_und_a', 'veth_und_b', 'br_und'))):
            for interface in interfaces:
                D.sh('ip', 'link', 'set', interface, 'mtu', '1280', ns=site, check_rc=True)
                D.sh('ethtool', '-K', interface, 'tx', 'off', 'rx', 'off', 'tso', 'off',
                     'gso', 'off', 'gro', 'off', ns=site)
        for site, prefix in (('gwa', '10.77.2.0/24'), ('gwb', '10.77.1.0/24')):
            D.sh('ip', 'link', 'add', 'vpn0', 'type', 'vpt1_ay1', ns=site, check_rc=True)
            D.sh('ip', 'link', 'set', 'vpn0', 'up', ns=site, check_rc=True)
            D.sh('ip', 'route', 'add', prefix, 'dev', 'vpn0', ns=site, check_rc=True)
            D.sh('sysctl', '-q', '-w', 'net.ipv4.conf.all.rp_filter=0', ns=site, check_rc=True)
        ga, gb = D.Gateway('gwa', D.ID_A), D.Gateway('gwb', D.ID_B)
        gateways += [ga, gb]
        for gw, lan, addr, outer, remote, peer_lan, direction, own, peer in (
                (ga, '10.77.1.0', '10.77.1.1', '172.31.250.11', '172.31.250.12',
                 '10.77.2.0', 1, 16, 32),
                (gb, '10.77.2.0', '10.77.2.1', '172.31.250.12', '172.31.250.11',
                 '10.77.1.0', 2, 32, 16)):
            snap = D.snapshot(gw.lan_if, gw.und_if, lan, addr, outer, remote, peer_lan,
                              [(1, direction, 17, 9090, SRC, DST)], own, peer, 1280)
            D.prepare(gw, snap)
            link = json.loads(D.sh('ip', '-j', 'link', 'show', 'vpn0', ns=gw.site,
                                   check_rc=True).stdout)[0]
            check(f'{gw.site}-committed-mtu', link['mtu'] == 1208)
        D.finish(ga, D.K1, D.K2, gb.local_rx)
        D.finish(gb, D.K2, D.K1, ga.local_rx)
        D.wait_active(ga, gb)
        check('real-netlink-provisioning', D.failed == 0)
        with D.netns('hb'):
            receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(receiver)
            receiver.bind((DST, 9090))
            receiver.settimeout(1)
        with D.netns('ha'):
            sender = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
            sockets.append(sender)
        outer_file, lan_file = directory / 'outer.pcap', directory / 'lan-a.pcap'
        with D.capture('und', 'veth_und_a', str(outer_file)), D.capture('ha', 'veth_ha', str(lan_file)):
            for size in (40, 1207, 1208):
                payload = (f'hr4-mtu-{size}-'.encode() + b'x' * size)[:size - 28]
                sender.sendto(D.raw_udp(SRC, DST, payload, sport=23456, dport=9090), (DST, 0))
                data, address = receiver.recvfrom(2048)
                check(f'inner-{size}-arrives-intact', data == payload and address[0] == SRC)
            oversized = D.raw_udp(SRC, DST, b'over-limit-' + b'z' * 1170, sport=23456, dport=9090)
            check('oversize-is-exactly-m-plus-one', len(oversized) == 1209)
            start = ga.counters()
            sender.sendto(oversized, (DST, 0))
            try:
                receiver.recvfrom(2048)
                delivered = True
            except socket.timeout:
                delivered = False
            finish = ga.counters()
            check('m-plus-one-never-delivered', not delivered)
            check('ptb-before-tunnel-xmit', start['TX_PACKETS'] == finish['TX_PACKETS'] and
                  start['TX_DROPPED'] == finish['TX_DROPPED'])
            healthy = b'healthy-after-ptb'
            sender.sendto(D.raw_udp(SRC, DST, healthy, sport=23456, dport=9090), (DST, 0))
            check('healthy-service-after-ptb', receiver.recvfrom(2048)[0] == healthy)
        outer = [p[14:] for p in D.read_pcap(outer_file) if len(p) >= 42 and
                 p[12:14] == b'\x08\0' and p[23] == 17 and
                 p[26:30] == socket.inet_aton('172.31.250.11')]
        check('carrier-boundaries', {1279, 1280} <= {len(p) for p in outer})
        check('carrier-never-fragmented-or-oversize', bool(outer) and all(
            len(p) <= 1280 and struct.unpack_from('>H', p, 6)[0] == 0x4000 for p in outer))
        check('carrier-checksums-valid', bool(outer) and all(D.csum(p[:20]) == 0 and
              D.csum(p[12:20] + struct.pack('>BBH', 0, 17, len(p) - 20) + p[20:]) == 0
              for p in outer))
        errors = [p[14:] for p in D.read_pcap(lan_file) if len(p) >= 70 and
                  p[12:14] == b'\x08\0' and p[23] == 1 and p[34:36] == b'\x03\x04']
        check('one-local-ptb-with-correct-mtu', len(errors) == 1 and
              struct.unpack_from('>H', errors[0], 26)[0] == 1208)
        check('ptb-checksums-and-addresses', len(errors) == 1 and
              errors[0][12:20] == socket.inet_aton('10.77.1.1') + socket.inet_aton(SRC) and
              D.csum(errors[0][:20]) == 0 and D.csum(errors[0][20:]) == 0)
        check('ptb-quotes-exact-original', len(errors) == 1 and all(
            errors[0][28 + i] == oversized[i] for i in range(28) if i not in (8, 10, 11)))

        # A deletion drains gateway handler references under RTNL. A COMMIT
        # must never retain such a reference while waiting to acquire RTNL.
        ga.stop.set()
        ga.thread.join(timeout=3)
        racing = threading.Event()
        ready = threading.Event()
        completed = []

        def commits():
            try:
                ready.set()
                while not racing.is_set():
                    completed.append(ga.cmd('COMMIT_CONFIG', D.G.a('CONFIG_REVISION', D.G.u64(2)))[0])
            except OSError:
                pass

        writer = threading.Thread(target=commits, daemon=True)
        writer.start()
        ready.wait(timeout=2)
        time.sleep(0.05)
        try:
            result = D.sh('ip', 'link', 'del', 'vpn0', ns='gwa', timeout=10)
        finally:
            racing.set()
            writer.join(timeout=6)
        check('commit-races-device-removal-without-deadlock', result.returncode == 0 and
              not writer.is_alive() and bool(completed))
        report['status'] = 'PASS' if all(case['passed'] for case in report['cases']) else 'FAIL'
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        for gw in gateways:
            gw.stop.set()
            if gw.thread:
                gw.thread.join(timeout=3)
            gw.owner.close()
            gw.observer.close()
        for sock in sockets:
            sock.close()
        result = subprocess.run(['bash', str(Path(D.HERE) / 'n1_fixture.sh'), 'teardown', D.RUN],
                                capture_output=True, text=True, timeout=60)
        check('fixture-cleanup', result.returncode == 0)
        report['root_network_unchanged'] = before == root_network()
        if not report['root_network_unchanged'] or not all(c['passed'] for c in report['cases']):
            report['status'] = 'FAIL'
        report['capture_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                    for p in sorted(directory.glob('*.pcap'))}
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HR4_MTU {report['status']} cases={len(report['cases'])}", flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
