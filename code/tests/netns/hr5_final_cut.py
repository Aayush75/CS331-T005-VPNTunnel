#!/usr/bin/env python3
"""Pause real TX after crypto, tighten/revoke, prove no stale ciphertext is emitted.

Debug foundation only; the root-only bounded delay grants no permissions and is
absent from release. Every failure restores it. The timing precondition must
pass; an unachieved race is not counted as a successful negative.
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
PARAMS = Path('/sys/module/vpt1_foundation/parameters')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or os.environ.get('VPT1_DISPOSABLE_VM') != 'yes':
        parser.error('requires root and VPT1_DISPOSABLE_VM=yes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(status='ERROR', cases=[], schedule={}, scope='real post-crypto TX final cut')
    gateways, sockets, workers = [], [], []
    delay, seen = PARAMS / 'tx_test_delay_us', PARAMS / 'tx_test_seen'
    before = root_network()
    captures = []

    def check(name, condition):
        report['cases'].append(dict(name=name, passed=bool(condition)))
        if not condition:
            print(f'FAIL {name}', flush=True)

    try:
        if not delay.exists() or not seen.exists():
            raise RuntimeError('debug TX observation parameters missing')
        delay.write_text('0\n')
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

        def snap(gw, port):
            a = gw.site == 'gwa'
            return D.snapshot(gw.lan_if, gw.und_if, '10.77.1.0' if a else '10.77.2.0',
                              '10.77.1.1' if a else '10.77.2.1',
                              '172.31.250.11' if a else '172.31.250.12',
                              '172.31.250.12' if a else '172.31.250.11',
                              '10.77.2.0' if a else '10.77.1.0',
                              [(1, 1 if a else 2, 17, port, SRC, DST)],
                              16 if a else 32, 32 if a else 16)

        D.prepare(ga, snap(ga, 9090))
        D.prepare(gb, snap(gb, 9090))
        D.finish(ga, D.K1, D.K2, gb.local_rx)
        D.finish(gb, D.K2, D.K1, ga.local_rx)
        D.wait_active(ga, gb)
        check('real-netlink-provisioning', D.failed == 0)
        # One writer owns the owner socket during the race; lease stays live.
        ga.stop.set()
        ga.thread.join(timeout=3)
        with D.netns('ha'):
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sender)
        with D.netns('hb'):
            receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            receiver.bind((DST, 9090))
            receiver.settimeout(0.3)
            sockets.append(receiver)

        def healthy(label):
            value = label.encode() + os.urandom(16)
            sender.sendto(value, (DST, 9090))
            data, address = receiver.recvfrom(2048)
            check(label, data == value and address[0] == SRC)

        def stats():
            result = ga.counters()
            err, reply = ga.observer.cmd('COUNTERS', D.G.a('IFINDEX', D.G.u32(ga.ifx)))
            if err:
                raise RuntimeError(f'COUNTERS: {err}')
            values = D.G.parse_attrs(reply[D.G.A['COUNTERS']][0])
            result['ACL_DENY'] = struct.unpack('=Q', values[10][0])[0]
            result['PEER_INELIGIBLE'] = struct.unpack('=Q', values[6][0])[0]
            return result

        def commit(revision, port):
            for name, attrs in (
                    ('VALIDATE', [D.G.a('CONFIG_REVISION', D.G.u64(revision)),
                                  D.G.attr(D.G.A['SNAPSHOT'], snap(ga, port), nested=True)]),
                    ('COMMIT_CONFIG', [D.G.a('CONFIG_REVISION', D.G.u64(revision))])):
                err = ga.cmd(name, *attrs)[0]
                check(f'{name}-{revision}', err == 0)
                if err:
                    raise RuntimeError(f'{name}: {err}')

        def cut(label, action, reason):
            path = args.output.parent / f'{args.output.stem}-{label}.pcap'
            captures.append(path)
            start = stats()
            marker = int(seen.read_text())
            send_errors = []

            def send():
                try:
                    sender.sendto(label.encode() + os.urandom(16), (DST, 9090))
                except OSError as exc:
                    send_errors.append(str(exc))

            with D.capture('und', 'veth_und_a', str(path)):
                delay.write_text('100000\n')
                started = time.monotonic()
                worker = threading.Thread(target=send, daemon=True)
                workers.append(worker)
                worker.start()
                limit = started + 0.5
                while int(seen.read_text()) == marker and time.monotonic() < limit:
                    time.sleep(0.001)
                observed = int(seen.read_text()) > marker
                invoked = time.monotonic() - started
                check(f'{label}-post-crypto-schedule-reached', observed and invoked < 0.08)
                action()
                completed = time.monotonic() - started
                if label == 'policy':
                    check('policy-commit-ack-before-pause-ended', completed < 0.08)
                worker.join(timeout=2)
                delay.write_text('0\n')
                check(f'{label}-sender-completes', not worker.is_alive() and not send_errors)
                try:
                    receiver.recvfrom(2048)
                    delivered = True
                except socket.timeout:
                    delivered = False
                finish = stats()
                check(f'{label}-no-stale-inner-delivery', not delivered)
                check(f'{label}-exact-final-drop-reason',
                      finish['TX_DROPPED'] == start['TX_DROPPED'] + 1 and
                      finish[reason] == start[reason] + 1 and
                      finish['TX_PACKETS'] == start['TX_PACKETS'])
            carriers = [p for p in D.read_pcap(path) if len(p) >= 42 and
                        p[12:14] == b'\x08\0' and p[23] == 17 and
                        p[26:30] == socket.inet_aton('172.31.250.11') and
                        struct.unpack_from('>H', p, 16)[0] > 88]
            check(f'{label}-no-stale-ciphertext-emitted', not carriers)
            report['schedule'][label] = dict(observed=observed, invoked_seconds=invoked,
                                             completed_seconds=completed, pause_seconds=0.1)

        healthy('healthy-before-policy-cut')
        cut('policy', lambda: commit(2, 9091), 'ACL_DENY')
        commit(3, 9090)
        healthy('healthy-after-policy-restore')

        def revoke():
            err = ga.cmd('REVOKE', D.G.a('PEER_SLOT', D.G.u16(1)),
                         D.G.a('PEER_INCARNATION', ga.incarnation()))[0]
            check('revoke-ack', err == 0)
        cut('revoke', revoke, 'PEER_INELIGIBLE')
        report['status'] = 'PASS' if all(c['passed'] for c in report['cases']) else 'FAIL'
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        if delay.exists():
            delay.write_text('0\n')
        for worker in workers:
            worker.join(timeout=2)
        check('send-workers-drained', not any(w.is_alive() for w in workers))
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
        report['capture_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                    for p in captures if p.exists()}
        if not report['root_network_unchanged'] or not all(c['passed'] for c in report['cases']):
            report['status'] = 'FAIL'
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HR5_FINAL_CUT {report['status']} cases={len(report['cases'])}", flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
