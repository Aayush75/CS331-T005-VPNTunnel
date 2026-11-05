#!/usr/bin/env python3
"""Hold authenticated RX across a real lease deadline; stale intake time must not admit it.

Debug-only delay and cleanup stall, real Netlink, kernel crypto and application
traffic. No clock/deadline is changed. A missed timing precondition fails.
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


def boot_time():
    return time.clock_gettime(time.CLOCK_BOOTTIME)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or os.environ.get('VPT1_DISPOSABLE_VM') != 'yes':
        parser.error('requires root and VPT1_DISPOSABLE_VM=yes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(status='ERROR', cases=[], scope='real RX final lease deadline')
    before = root_network()
    gateways, sockets, workers = [], [], []
    delay, seen = PARAMS / 'rx_test_delay_us', PARAMS / 'rx_test_seen'
    stall = PARAMS / 'debug_stall_expiry'
    capture = args.output.with_suffix('.pcap')

    def check(name, condition):
        report['cases'].append(dict(name=name, passed=bool(condition)))
        if not condition:
            print(f'FAIL {name}', flush=True)

    try:
        if not all(p.exists() for p in (delay, seen, stall)):
            raise RuntimeError('debug RX/expiry parameters missing')
        delay.write_text('0\n')
        stall.write_text('0\n')
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

        def snap(gw):
            a = gw.site == 'gwa'
            return D.snapshot(gw.lan_if, gw.und_if, '10.77.1.0' if a else '10.77.2.0',
                              '10.77.1.1' if a else '10.77.2.1',
                              '172.31.250.11' if a else '172.31.250.12',
                              '172.31.250.12' if a else '172.31.250.11',
                              '10.77.2.0' if a else '10.77.1.0',
                              [(1, 1 if a else 2, 17, 9090, SRC, DST)],
                              16 if a else 32, 32 if a else 16)

        D.prepare(ga, snap(ga))
        D.prepare(gb, snap(gb))
        D.finish(ga, D.K1, D.K2, gb.local_rx)
        D.finish(gb, D.K2, D.K1, ga.local_rx)
        D.wait_active(ga, gb)
        check('real-netlink-provisioning', D.failed == 0)
        with D.netns('ha'):
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sender)
        with D.netns('hb'):
            receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            receiver.bind((DST, 9090))
            receiver.settimeout(0.3)
            sockets.append(receiver)
        time.sleep(0.3)  # carrier/qdisc attachment

        def healthy(label):
            value = label.encode() + os.urandom(16)
            sender.sendto(value, (DST, 9090))
            data, source = receiver.recvfrom(2048)
            check(label, data == value and source[0] == SRC)

        def stats():
            values = gb.counters()
            err, reply = gb.observer.cmd('COUNTERS', D.G.a('IFINDEX', D.G.u32(gb.ifx)))
            if err:
                raise RuntimeError(f'COUNTERS: {err}')
            nested = D.G.parse_attrs(reply[D.G.A['COUNTERS']][0])
            values['LEASE_EXPIRED'] = struct.unpack('=Q', nested[4][0])[0]
            return values

        healthy('healthy-before-expiry')
        old_inc = gb.incarnation()
        gb.stop.set()
        gb.thread.join(timeout=3)
        check('receiver-renewal-thread-drained', not gb.thread.is_alive())
        if gb.thread.is_alive():
            raise RuntimeError('receiver owner socket is still in use')
        # Cleanup must not erase the peer first: this tests the admission clock
        # comparison itself, not a timer-driven INVALID/NO_OWNER rejection.
        stall.write_text('1\n')
        earliest = boot_time() + 5
        err = gb.cmd('RENEW', D.G.a('CONFIG_REVISION', D.G.u64(1)))[0]
        latest = boot_time() + 5
        check('real-renewal-with-bounded-clock-bracket', err == 0 and latest - earliest < 0.02)
        if err or latest - earliest >= 0.02:
            raise RuntimeError('renewal clock bracket not achieved')
        baseline = stats()
        with D.capture('und', 'veth_und_b', str(capture)):
            time.sleep(max(0, earliest - 0.06 - boot_time()))
            marker = int(seen.read_text())
            delay.write_text('100000\n')
            send_errors = []

            def send():
                try:
                    sender.sendto(b'expired-rx-' + os.urandom(16), (DST, 9090))
                except OSError as exc:
                    send_errors.append(str(exc))

            worker = threading.Thread(target=send, daemon=True)
            workers.append(worker)
            worker.start()
            limit = time.monotonic() + 0.3
            while int(seen.read_text()) == marker and time.monotonic() < limit:
                time.sleep(0.001)
            observed = boot_time()
            reached = int(seen.read_text()) > marker
            check('authenticated-rx-paused-before-earliest-deadline', reached and observed < earliest)
            time.sleep(max(0, latest + 0.15 - boot_time()))
            worker.join(timeout=2)
            delay.write_text('0\n')
            check('sender-completed', not worker.is_alive() and not send_errors)
            try:
                receiver.recvfrom(2048)
                delivered = True
            except socket.timeout:
                delivered = False
            finish = stats()
            check('expired-rx-never-delivered', not delivered)
            check('exact-final-lease-rejection',
                  finish['LEASE_EXPIRED'] == baseline['LEASE_EXPIRED'] + 1 and
                  finish['RX_POLICY'] == baseline['RX_POLICY'] + 1 and
                  finish['RX_PACKETS'] == baseline['RX_PACKETS'])
        data_records = [p for _, _, p in D.udp_datagrams(D.read_pcap(capture))
                        if len(p) >= 64 and p[:5] == b'VPT1\x03']
        check('real-carrier-reached-receiver', len(data_records) == 1)
        report['schedule'] = dict(earliest_deadline=earliest, latest_deadline=latest,
                                  observed_pause=observed, pause_seconds=0.1)
        stall.write_text('0\n')
        # Fresh public fixture keys/locators: recovery does not reset counters
        # under the previous key or resurrect its old peer incarnation.
        gb.stop = threading.Event()
        k1, k2 = bytes([91]) * 32, bytes([92]) * 32
        D.prepare(gb, snap(gb))
        check('fresh-owner-has-new-incarnation', gb.incarnation() != old_inc and D.failed == 0)
        # D.Gateway has one owner socket, so don't overlap this install with
        # its renewal thread's request/reply exchange.
        ga.stop.set()
        ga.thread.join(timeout=3)
        if ga.thread.is_alive():
            raise RuntimeError('sender owner socket is still in use')
        err = ga.cmd('RENEW', D.G.a('CONFIG_REVISION', D.G.u64(1)))[0]
        if err:
            raise RuntimeError(f'sender renewal before recovery: {err}')
        new_inc, new_attempt, new_local = ga.begin_attempt()
        activated = {g.site: g.counters()['ACTIVATED'] for g in (ga, gb)}
        D.finish(gb, k2, k1, new_local)
        # Both kernels must hold the same handshake hash for CONFIRM to match (D.finish stages b'H' * 32).
        err = ga.cmd('STAGE_GENERATION', D.G.a('PEER_SLOT', D.G.u16(1)),
                     D.G.a('PEER_INCARNATION', new_inc),
                     D.G.a('ATTEMPT_ID', new_attempt), D.G.a('HANDSHAKE_HASH', b'H' * 32),
                     D.G.a('LOCAL_RX', D.G.u64(new_local)), D.G.a('REMOTE_RX', D.G.u64(gb.local_rx)),
                     D.G.a('TX_KEY', k1), D.G.a('RX_KEY', k2), D.G.a('MODE', b'\0'))[0]
        check('fresh-sender-generation-installed', err == 0)
        ga.stop = threading.Event()
        ga.start_renewals()
        D.wait_active(ga, gb, before=activated)
        time.sleep(2.2)
        healthy('healthy-after-fresh-establishment')
        report['status'] = 'PASS' if all(c['passed'] for c in report['cases']) else 'FAIL'
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        for parameter in (delay, stall):
            if parameter.exists():
                parameter.write_text('0\n')
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
        if capture.exists():
            report['capture_sha256'] = hashlib.sha256(capture.read_bytes()).hexdigest()
        if not report['root_network_unchanged'] or not all(c['passed'] for c in report['cases']):
            report['status'] = 'FAIL'
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HR5_RX_DEADLINE {report['status']} cases={len(report['cases'])}", flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
