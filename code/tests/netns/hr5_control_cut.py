#!/usr/bin/env python3
"""Revoke a real receive-side PONG after crypto and before UDP output.

The injected authenticated PING uses public fixture keys. AAD/counter plus an
independent PONG ciphertext oracle distinguish its reply from background PINGs.
The namespace-scoped delay only stalls debug output; it grants no permission.
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
from hr4_ip import root_network, encrypt

SRC, DST = '10.77.1.10', '10.77.2.10'
OA, OB = '172.31.250.11', '172.31.250.12'
PARAMS = Path('/sys/module/vpt1_foundation/parameters')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or os.environ.get('VPT1_DISPOSABLE_VM') != 'yes':
        parser.error('requires root and VPT1_DISPOSABLE_VM=yes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(status='ERROR', cases=[], scope='real PONG post-crypto revoke cut')
    gateways, sockets, workers = [], [], []
    before = root_network()
    delay, seen = PARAMS / 'control_test_delay_us', PARAMS / 'control_test_seen'
    target = PARAMS / 'control_test_netns'
    capture = args.output.with_suffix('.pcap')

    def check(name, condition):
        report['cases'].append(dict(name=name, passed=bool(condition)))
        if not condition:
            print(f'FAIL {name}', flush=True)

    try:
        if not all(p.exists() for p in (delay, seen, target)):
            raise RuntimeError('debug control parameters missing')
        delay.write_text('0\n')
        target.write_text('0\n')
        result = subprocess.run(['bash', str(Path(D.HERE) / 'n1_fixture.sh'), 'setup', D.RUN],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        for site, prefix in (('gwa', '10.77.2.0/24'), ('gwb', '10.77.1.0/24')):
            D.sh('ip', 'link', 'add', 'vpn0', 'type', 'vpt1_ay1', ns=site, check_rc=True)
            D.sh('ip', 'link', 'set', 'vpn0', 'up', ns=site, check_rc=True)
            D.sh('ip', 'route', 'add', prefix, 'dev', 'vpn0', ns=site, check_rc=True)
            D.sh('sysctl', '-q', '-w', 'net.ipv4.conf.all.rp_filter=0', ns=site, check_rc=True)
        ga, gb = D.Gateway('gwa', D.ID_A), D.Gateway('gwb', D.ID_B)
        gateways += [ga, gb]
        for gw, lan, addr, outer, remote, peer_lan, direction, own, peer in (
                (ga, '10.77.1.0', '10.77.1.1', OA, OB, '10.77.2.0', 1, 16, 32),
                (gb, '10.77.2.0', '10.77.2.1', OB, OA, '10.77.1.0', 2, 32, 16)):
            D.prepare(gw, D.snapshot(gw.lan_if, gw.und_if, lan, addr, outer, remote, peer_lan,
                                    [(1, direction, 17, 9090, SRC, DST)], own, peer))
        D.finish(ga, D.K1, D.K2, gb.local_rx)
        D.finish(gb, D.K2, D.K1, ga.local_rx)
        D.wait_active(ga, gb)
        check('kernel-issued-attempts-and-generations', D.failed == 0)
        with D.netns('ha'):
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sender)
        with D.netns('hb'):
            receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            receiver.bind((DST, 9090))
            receiver.settimeout(1)
            sockets.append(receiver)
        with D.netns('gwb'):
            injector = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
            injector.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
            sockets.append(injector)
        time.sleep(0.3)
        healthy = b'control-cut-healthy-' + os.urandom(16)
        sender.sendto(healthy, (DST, 9090))
        data, source = receiver.recvfrom(2048)
        check('healthy-data-before-control-cut', data == healthy and source[0] == SRC)
        challenge = os.urandom(16)
        aad = b'VPT1\x03\0' + struct.pack('>HQQ', 44, ga.local_rx, 511)
        ping = aad + encrypt(D.K2, b'\0' * 4 + struct.pack('<Q', 511), aad,
                             b'\x06\0\0\x10' + challenge)
        wire = D.raw_udp(OB, OA, ping)
        errors = []

        def inject():
            try:
                injector.sendto(wire, (OA, 0))
            except OSError as exc:
                errors.append(str(exc))

        def stats():
            err, reply = ga.observer.cmd('COUNTERS', D.G.a('IFINDEX', D.G.u32(ga.ifx)))
            if err:
                raise RuntimeError(f'COUNTERS: {err}')
            nested = D.G.parse_attrs(reply[D.G.A['COUNTERS']][0])
            return {name: struct.unpack('=Q', nested[index][0])[0]
                    for name, index in (('peer_ineligible', 6), ('tx_pong', 39))}

        with D.capture('und', 'veth_und_a', str(capture)):
            target.write_text(str(Path('/var/run/netns', D.NS['gwa']).stat().st_ino) + '\n')
            delay.write_text('100000\n')
            marker, start = int(seen.read_text()), stats()
            worker = threading.Thread(target=inject, daemon=True)
            workers.append(worker)
            started = time.monotonic()
            worker.start()
            limit = started + 0.5
            while int(seen.read_text()) == marker and time.monotonic() < limit:
                time.sleep(0.001)
            reached = int(seen.read_text()) > marker
            invoked = time.monotonic() - started
            check('authenticated-pong-post-crypto-schedule', reached and invoked < 0.08)
            err = ga.cmd('REVOKE', D.G.a('PEER_SLOT', D.G.u16(1)),
                         D.G.a('PEER_INCARNATION', ga.incarnation()))[0]
            check('revoke-completes-after-processing-drain', err == 0)
            worker.join(timeout=2)
            delay.write_text('0\n')
            target.write_text('0\n')
            check('injector-completes', not worker.is_alive() and not errors)
            finish = stats()
            check('control-final-cut-rejects-old-peer',
                  finish['peer_ineligible'] > start['peer_ineligible'] and
                  finish['tx_pong'] == start['tx_pong'])
        matching = []
        for src, dst, payload in D.udp_datagrams(D.read_pcap(capture)):
            if src != OA or dst != OB or len(payload) != 60 or payload[:5] != b'VPT1\x03':
                continue
            counter = struct.unpack_from('>Q', payload, 16)[0]
            expected = payload[:24] + encrypt(D.K1, b'\0' * 4 + struct.pack('<Q', counter),
                                               payload[:24], b'\x07\0\0\x10' + challenge)
            if payload == expected:
                matching.append(counter)
        check('no-stale-authenticated-pong-on-wire', not matching)
        report['schedule'] = dict(observed=reached, invoked_seconds=invoked, pause_seconds=0.1)
        report['status'] = 'PASS' if all(c['passed'] for c in report['cases']) else 'FAIL'
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        for parameter in (delay, target):
            if parameter.exists():
                parameter.write_text('0\n')
        for worker in workers:
            worker.join(timeout=2)
        check('injector-threads-drained', not any(w.is_alive() for w in workers))
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
        if capture.exists():
            report['capture_sha256'] = hashlib.sha256(capture.read_bytes()).hexdigest()
        if not report['root_network_unchanged'] or not all(c['passed'] for c in report['cases']):
            report['status'] = 'FAIL'
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HR5_CONTROL_CUT {report['status']} cases={len(report['cases'])}", flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
