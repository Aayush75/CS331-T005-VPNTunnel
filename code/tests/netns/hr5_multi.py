#!/usr/bin/env python3
"""HR5: real N2 multi-peer policy/rekey/revoke traffic through the kernel.

Uses public deterministic conformance keys installed through real Netlink,
not a userspace packet protector. This qualifies the current synchronous path;
it does not substitute for live witness/conntrack or future worker accounting.
"""
import argparse
import contextlib
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

G = D.G
SITES = dict(gwa=(1, 16), gwb=(2, 32), gwc=(3, 48))
MARKER = b'hr5-multi-public-plaintext-marker-2026'


class Gateway(D.Gateway):
    def __init__(self, site):
        self.site = site
        _, seed = SITES[site]
        self.cid = bytes([seed]) * 16
        with D.netns(site):
            self.owner, self.observer = G.Genl(), G.Genl()
            self.ifx = socket.if_nametoindex('vpn0')
            self.lan_if = socket.if_nametoindex(f'veth_{site}_lan')
            self.und_if = socket.if_nametoindex(f'veth_{site}_out')
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.thread = None
        self.revision = 0
        self.renew_errors = []

    def cmd(self, name, *attrs):
        with self.lock:
            return super().cmd(name, *attrs)

    def require(self, name, *attrs):
        err, reply = self.cmd(name, *attrs)
        if err:
            raise RuntimeError(f'{self.site} {name}: {err}')
        return reply

    def commit(self, snapshot):
        with self.lock:
            revision = self.revision + 1
            self.require('VALIDATE', G.a('CONFIG_REVISION', G.u64(revision)),
                         G.attr(G.A['SNAPSHOT'], snapshot, nested=True))
            self.require('COMMIT_CONFIG', G.a('CONFIG_REVISION', G.u64(revision)))
            self.revision = revision

    def incarnation(self, slot=1):
        err, status = G.status(self.observer, self.ifx)
        if err:
            raise RuntimeError(f'{self.site} status: {err}')
        return status['peers'][slot]['inc']

    def start_renewals(self):
        def renew():
            while not self.stop.wait(0.5):
                with self.lock:
                    err, _ = self.cmd('RENEW', G.a('CONFIG_REVISION', G.u64(self.revision)))
                    if err:
                        self.renew_errors.append(err)
        self.thread = threading.Thread(target=renew, daemon=True)
        self.thread.start()


def snapshot(gw, allow_c=True):
    number, own_seed = SITES[gw.site]
    local = (struct.pack('>II', gw.lan_if, gw.und_if) + D.pfx(f'10.77.{number}.0', 24) +
             D.pfx('172.31.250.0', 24) + socket.inet_aton(f'10.77.{number}.1') +
             socket.inet_aton(f'172.31.250.{10 + number}') + D.key(own_seed))
    result = G.attr(1, local)
    peers = ((1, 'gwa', 1500), (2, 'gwc', 1280)) if gw.site == 'gwb' else (
        (1, 'gwb', 1280 if gw.site == 'gwc' else 1500),)
    for slot, remote, pmtu in peers:
        peer_number, peer_seed = SITES[remote]
        rec = (struct.pack('>BBH', slot, 1 if own_seed < peer_seed else 0, pmtu) + socket.inet_aton(f'172.31.250.{10 + peer_number}') +
               struct.pack('>H', 51888) + D.key(peer_seed) + bytes([1]) +
               D.pfx(f'10.77.{peer_number}.0', 24) + b'\0' * 15)
        result += G.attr(2, rec)
        source_number = peer_number if gw.site == 'gwb' else number
        port = 9091 if gw.site == 'gwb' and slot == 2 and not allow_c else 9090
        direction = 2 if gw.site == 'gwb' else 1
        result += G.attr(3, struct.pack('>IBBBHH', slot, slot, direction, 17, port, port) +
                         D.pfx(f'10.77.{source_number}.10', 32) + D.pfx('10.77.2.10', 32))
    return result


def install(gw, slot, tx, rx, attempt, remote):
    inc, attempt_id, local = attempt
    gw.require('STAGE_GENERATION', G.a('PEER_SLOT', G.u16(slot)),
               G.a('PEER_INCARNATION', inc), G.a('ATTEMPT_ID', attempt_id),
               G.a('HANDSHAKE_HASH', b'H' * 32), G.a('LOCAL_RX', G.u64(local)),
               G.a('REMOTE_RX', G.u64(remote)), G.a('TX_KEY', tx), G.a('RX_KEY', rx),
               G.a('MODE', b'\0'))


def rekey(leaf, hub, n):
    c = leaf.site == 'gwc'
    slot = 2 if c else 1
    hub_attempt = hub.begin_attempt(slot)
    leaf_attempt = leaf.begin_attempt()
    seed = (b'public-hr5-C-' if c else b'public-hr5-A-') + struct.pack('>I', n)
    tx, rx = hashlib.sha256(seed + b'tx').digest(), hashlib.sha256(seed + b'rx').digest()
    before = {g.site: g.counters()['ACTIVATED'] for g in (leaf, hub)}
    install(hub, slot, rx, tx, hub_attempt, leaf_attempt[2])
    install(leaf, 1, tx, rx, leaf_attempt, hub_attempt[2])
    D.wait_active(leaf, hub, before=before)
    return hub_attempt[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or os.environ.get('VPT1_DISPOSABLE_VM') != 'yes':
        parser.error('requires root and VPT1_DISPOSABLE_VM=yes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    D.NS.update({site: f'vpt_{D.RUN}_{site}' for site in ('gwc', 'hc')})
    report = dict(status='ERROR', cases=[], scope='synchronous N2 kernel path, test-installed keys')
    gateways, sockets, threads = [], [], []
    captures = contextlib.ExitStack()
    wire = args.output.parent / (args.output.stem + '-outer.pcap')
    stop = threading.Event()
    state_lock = threading.Lock()
    counts = dict(a_ok=0, a_lost=0, corrupt=0, wrong_source=0)
    before = root_network()

    def check(name, condition, detail=None):
        report['cases'].append(dict(name=name, passed=bool(condition), detail=detail))
        if not condition:
            print(f'FAIL {name} {detail or ""}', flush=True)

    try:
        result = subprocess.run(['bash', str(Path(D.HERE) / 'n2_fixture.sh'), 'setup', D.RUN],
                                capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        D.sh('ip', 'route', 'add', '10.77.3.0/24', 'via', '10.77.2.1', ns='hb', check_rc=True)
        for site, targets in (('gwa', (2,)), ('gwb', (1, 3)), ('gwc', (2,))):
            D.sh('ip', 'link', 'add', 'vpn0', 'type', 'vpt1_ay1', ns=site, check_rc=True)
            D.sh('ip', 'link', 'set', 'vpn0', 'up', ns=site, check_rc=True)
            D.sh('sysctl', '-q', '-w', 'net.ipv4.conf.all.rp_filter=0', ns=site, check_rc=True)
            D.sh('ethtool', '-K', f'veth_{site}_out', 'tx', 'off', 'rx', 'off',
                 'tso', 'off', 'gso', 'off', 'gro', 'off', ns=site)
            for target in targets:
                D.sh('ip', 'route', 'replace', f'10.77.{target}.0/24', 'dev', 'vpn0',
                     ns=site, check_rc=True)
        ga, gb, gc = (Gateway(site) for site in ('gwa', 'gwb', 'gwc'))
        gateways += [ga, gb, gc]
        for gw in gateways:
            gw.require('CLAIM')
            gw.commit(snapshot(gw))
        for gw in gateways:
            gw.start_renewals()  # Activation runs on the lease: renew before the first keys.
        rekey(ga, gb, 0)
        rekey(gc, gb, 0)
        a_locators, c_locators = set(), set()
        a_inc, c_inc = gb.incarnation(1), gb.incarnation(2)
        # Create all sockets serially before threads start; no concurrent setns.
        with D.netns('hb'):
            server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(server)
            server.bind(('10.77.2.10', 9090))
            server.settimeout(0.1)
        clients = {}
        for host, name in (('ha', 'A'), ('hc', 'C')):
            with D.netns(host):
                clients[name] = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                clients[name].settimeout(0.1)
                sockets.append(clients[name])
        captures.enter_context(D.capture('und', 'veth_und_b', str(wire)))

        def echo():
            while not stop.is_set():
                try:
                    data, address = server.recvfrom(2048)
                except socket.timeout:
                    continue
                who = data[len(MARKER):len(MARKER) + 1]
                expected = '10.77.1.10' if who == b'A' else '10.77.3.10'
                if not data.startswith(MARKER) or address[0] != expected:
                    with state_lock:
                        counts['wrong_source'] += 1
                server.sendto(data, address)

        def probe(name):
            payload = MARKER + name.encode() + os.urandom(16)
            sock = clients[name]
            sock.sendto(payload, ('10.77.2.10', 9090))
            end = time.monotonic() + 0.25
            while time.monotonic() < end:
                try:
                    data, address = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                if data == payload and address == ('10.77.2.10', 9090):
                    return True
                if not data.startswith(MARKER + name.encode()):
                    with state_lock:
                        counts['corrupt'] += 1
            return False

        def load_a():
            while not stop.is_set():
                good = probe('A')
                with state_lock:
                    counts['a_ok' if good else 'a_lost'] += 1
                stop.wait(0.01)

        for fn in (echo, load_a):
            thread = threading.Thread(target=fn, daemon=True)
            threads.append(thread)
            thread.start()
        time.sleep(0.3)
        check('both-peer-services-healthy', probe('C') and counts['a_ok'] > 0)
        for n in range(1, 5):
            allow_c = n % 2 == 0
            active_before = counts['a_ok']
            gb.commit(snapshot(gb, allow_c))
            check(f'round-{n}-policy-preserves-both-incarnations',
                  gb.incarnation(1) == a_inc and gb.incarnation(2) == c_inc)
            a_locators.add(rekey(ga, gb, n))
            c_locators.add(rekey(gc, gb, n))
            time.sleep(2.2)
            results = [probe('C') for _ in range(8)]
            check(f'round-{n}-fresh-c-policy', all(results) if allow_c else not any(results))
            check(f'round-{n}-a-progress-through-rekey-and-policy', counts['a_ok'] > active_before + 5)
        gb.require('REVOKE', G.a('PEER_SLOT', G.u16(2)), G.a('PEER_INCARNATION', c_inc))
        active_before = counts['a_ok']
        check('revoked-c-fresh-traffic-denied', not any(probe('C') for _ in range(4)))
        check('a-progress-during-c-revoke', counts['a_ok'] > active_before)
        gb.commit(snapshot(gb, True))
        check('c-slot-recreation-has-fresh-incarnation', gb.incarnation(2) != c_inc)
        check('a-incarnation-survives-c-recreation', gb.incarnation(1) == a_inc)
        c_locators.add(rekey(gc, gb, 5))
        time.sleep(2.2)
        check('c-recovers-only-with-fresh-generation', all(probe('C') for _ in range(8)))
        check('no-corruption-or-source-rewrite', not counts['corrupt'] and not counts['wrong_source'])
        check('owner-renewals-remain-valid', not any(gw.renew_errors for gw in gateways) and
              all(gw.thread.is_alive() for gw in gateways))
        stop.set()
        for thread in threads:
            thread.join(timeout=3)
        captures.close()
        locators = {}
        ipv4 = [p[14:] for p in D.read_pcap(wire) if len(p) >= 34 and p[12:14] == b'\x08\0']
        for packet in ipv4:
            if len(packet) < 52 or packet[9] != 17 or packet[28:32] != b'VPT1' or packet[32] != 3:
                continue
            locator, counter = struct.unpack_from('>QQ', packet, 36)
            locators.setdefault(locator, []).append(counter)
        check('a-used-every-new-generation-on-wire', len(a_locators) == 4 and a_locators <= set(locators))
        check('c-used-every-new-generation-on-wire', len(c_locators) == 5 and c_locators <= set(locators))
        check('record-counters-never-reused', bool(locators) and all(
            len(values) == len(set(values)) for values in locators.values()))
        # IGMP (protocol 2) membership reports from the bridge ports are link housekeeping, not carriers.
        strays = [p for p in ipv4 if p[9] != 2 and not (p[12:15] == b'\xac\x1f\xfa' and p[16:19] == b'\xac\x1f\xfa')]
        # The first offender's addresses and protocol make a rare stray packet diagnosable.
        check('underlay-only-carrier-addresses', bool(ipv4) and not strays,
              strays and dict(count=len(strays), src='.'.join(map(str, strays[0][12:16])),
                              dst='.'.join(map(str, strays[0][16:20])), proto=strays[0][9]))
        check('no-application-marker-in-clear', all(MARKER not in p for p in ipv4))
        report['wire_locator_records'] = {str(key): len(value) for key, value in locators.items()}
        report['traffic'] = dict(counts)
        report['status'] = 'PASS' if all(c['passed'] for c in report['cases']) else 'FAIL'
    except Exception as exc:
        report['error'] = str(exc)
    finally:
        stop.set()
        for thread in threads:
            thread.join(timeout=3)
        captures.close()
        check('traffic-threads-drained', not any(t.is_alive() for t in threads))
        for gw in gateways:
            gw.stop.set()
            if gw.thread:
                gw.thread.join(timeout=3)
            gw.owner.close()
            gw.observer.close()
        for sock in sockets:
            sock.close()
        result = subprocess.run(['bash', str(Path(D.HERE) / 'n2_fixture.sh'), 'teardown', D.RUN],
                                capture_output=True, text=True, timeout=90)
        check('fixture-cleanup', result.returncode == 0)
        report['root_network_unchanged'] = before == root_network()
        if wire.exists():
            report['capture_sha256'] = hashlib.sha256(wire.read_bytes()).hexdigest()
        if not report['root_network_unchanged'] or not all(c['passed'] for c in report['cases']):
            report['status'] = 'FAIL'
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HR5_MULTI {report['status']} cases={len(report['cases'])}", flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
