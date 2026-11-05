#!/usr/bin/env python3
"""HR4 live IPv4 boundary cases; root, disposable 6.18 VM, foundation loaded.

Uses the AN2 N1 fixture, real Netlink provisioning and independent OpenSSL
records. A fresh UDP payload ID plus exact gateway counters and LAN captures
distinguish denial from a broken service. This qualifies the thin unfragmented
path only. Fragments, associated ICMP errors and bounded GSO are in hr3_live.py.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

import ay2_datapath as D

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from ay3_aead_vectors import encrypt  # noqa: E402

SRC, DST = '10.77.1.10', '10.77.2.10'
OA, OB = '172.31.250.11', '172.31.250.12'
ECN = ((0, 0, 0, None), (1, 1, 1, 3), (2, 1, 2, 3), (3, 3, 3, 3))


def root_network():
    """Read-only isolation observation; independent of the removed HR1 policy."""
    snapshot = {}
    for name, args in (('links', ['link']), ('routes4', ['route']), ('routes6', ['-6', 'route'])):
        command = ['ip', '-j', *args, 'show']
        if name != 'links':
            command += ['table', 'all']
        result = subprocess.run(command, capture_output=True, text=True, check=True, timeout=10)
        rows = json.loads(result.stdout)
        if name != 'links':
            rows = [{k: v for k, v in row.items() if k != 'expires'} for row in rows]
        snapshot[name] = rows
    return snapshot


def ip_header(src, dst, payload, tos=0, flags=0x4000, proto=17, options=b'', ident=0x7172):
    header = struct.pack('>BBHHHBBH4s4s', 0x45 + len(options) // 4, tos,
                         20 + len(options) + len(payload), ident, flags, 63, proto, 0,
                         socket.inet_aton(src), socket.inet_aton(dst)) + options
    return header[:10] + struct.pack('>H', D.csum(header)) + header[12:] + payload


def inner(payload, tos=0xa2, flags=0x4000):
    return ip_header(SRC, DST, D.raw_udp(SRC, DST, payload, 23456, 9090)[20:], tos, flags)


def protect(packet, counter):
    aad = b'VPT1\x03\0' + struct.pack('>HQQ', len(packet) + 28, D.GWB_RX[0], counter)
    plaintext = b'\x01\0' + struct.pack('>H', len(packet)) + packet
    return aad + encrypt(D.K1, b'\0' * 4 + struct.pack('<Q', counter), aad, plaintext)


def snmp_counter(protocol, name):
    text = D.sh('cat', '/proc/net/snmp', ns='gwb', check_rc=True).stdout.splitlines()
    for i, line in enumerate(text):
        if line.startswith(protocol + ':'):
            return dict(zip(line.split()[1:], map(int, text[i + 1].split()[1:])))[name]
    raise RuntimeError(f'missing {protocol} {name} counter')


def reassembly_requests():
    return snmp_counter('Ip', 'ReasmReqds')


def ethernet_send(packet):
    def mac(site, iface):
        result = D.sh('ip', '-j', 'link', 'show', iface, ns=site, check_rc=True)
        return bytes.fromhex(json.loads(result.stdout)[0]['address'].replace(':', ''))
    frame = mac('gwb', 'veth_gwb_out') + mac('gwa', 'veth_gwa_out') + b'\x08\0' + packet
    with D.netns('gwa'):
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800)) as sock:
            sock.bind(('veth_gwa_out', 0))
            sock.send(frame)


def counters(gw):
    result = gw.counters()
    err, reply = gw.observer.cmd('COUNTERS', D.G.a('IFINDEX', D.G.u32(gw.ifx)))
    if err:
        raise RuntimeError(f'COUNTERS failed: {err}')
    nested = D.G.parse_attrs(reply[D.G.A['COUNTERS']][0])
    result['RX_ECN_ANOMALY'] = struct.unpack('=Q', nested[36][0])[0]
    # Live IP adapter counters, appended after SIZE (member 50): members are index + 1.
    for name, member in (('FRAG_QUEUED', 51), ('FRAG_COMPLETE', 52), ('FRAG_DROPPED', 53),
                         ('FRAG_BUSY', 54), ('ERROR_ADMITTED', 55), ('ERROR_DENIED', 56),
                         ('TX_GSO_LIMIT', 57)):
        result[name] = struct.unpack('=Q', nested[member][0])[0] if member in nested else 0
    return result


class Campaign:
    def __init__(self, pmtu, directory):
        self.pmtu, self.directory = pmtu, directory
        self.results = []
        self.gateways = []
        self.sockets = []
        self.counter = 256
        self.serial = 0
        self.observations = {}

    def check(self, name, condition, detail=None):
        self.results.append(dict(name=name, passed=bool(condition), detail=detail))
        if not condition:
            print(f'FAIL P={self.pmtu} {name}: {detail}', flush=True)

    def payload(self, length=40):
        self.serial += 1
        value = f'hr4-{self.pmtu}-{self.serial:04d}-'.encode()
        return (value + b'z' * length)[:length]

    def receive(self, expected=None, tos=None):
        try:
            data, ancillary, _, addr = self.receiver.recvmsg(65535, 128)
        except socket.timeout:
            return expected is None
        if expected is None or data != expected or addr[0] != SRC:
            return False
        values = {kind: value for level, kind, value in ancillary if level == socket.IPPROTO_IP}
        return (values.get(socket.IP_TOS) == bytes([tos]) and
                struct.unpack('=i', values.get(socket.IP_TTL, b'\0' * 4))[0] == 62)

    def health(self, label):
        payload = self.payload()
        self.sender.sendto(payload, (DST, 9090))
        self.check(label, self.receive(payload, 0xa2))

    def inject(self, packet, outer=0, options=b''):
        self.counter += 1
        protected = protect(packet, self.counter)
        udp = D.raw_udp(OA, OB, protected)[20:]
        ethernet_send(ip_header(OA, OB, udp, outer, options=options))

    def exercise(self):
        # A deliberately larger physical test path lets the configured P=1500
        # receive boundary see an intact P+1 carrier instead of a veth send error.
        for site, interfaces in (('gwa', ('veth_gwa_out',)), ('gwb', ('veth_gwb_out',)),
                                 ('und', ('veth_und_a', 'veth_und_b', 'br_und'))):
            for iface in interfaces:
                D.sh('ip', 'link', 'set', iface, 'mtu', '1600', ns=site, check_rc=True)
                D.sh('ethtool', '-K', iface, 'tx', 'off', 'rx', 'off', 'tso', 'off',
                     'gso', 'off', 'gro', 'off', ns=site)
        for site in ('gwa', 'gwb'):
            D.sh('ip', 'link', 'add', 'vpn0', 'type', 'vpt1_ay1', ns=site, check_rc=True)
            D.sh('ip', 'link', 'set', 'vpn0', 'up', ns=site, check_rc=True)
            D.sh('sysctl', '-q', '-w', 'net.ipv4.conf.all.rp_filter=0', ns=site, check_rc=True)
        for site, prefix in (('gwa', '10.77.2.0/24'), ('gwb', '10.77.1.0/24')):
            D.sh('ip', 'route', 'add', prefix, 'dev', 'vpn0', ns=site, check_rc=True)
        ga, gb = D.Gateway('gwa', D.ID_A), D.Gateway('gwb', D.ID_B)
        self.gateways = [ga, gb]
        keys = {}
        for gw, local, remote, lan, address, peer_lan, direction, own, peer, tx, rx in (
                (ga, OA, OB, '10.77.1.0', '10.77.1.1', '10.77.2.0', 1, 16, 32, D.K1, D.K2),
                (gb, OB, OA, '10.77.2.0', '10.77.2.1', '10.77.1.0', 2, 32, 16, D.K2, D.K1)):
            snap = D.snapshot(gw.lan_if, gw.und_if, lan, address, local, remote, peer_lan,
                              [(1, direction, 17, 9090, SRC, DST)], own, peer, self.pmtu)
            target_mtu = self.pmtu - 72
            if gw.site == 'gwb' and self.pmtu == 1500:
                # A second configured relationship lowers only shared TX MTU.
                # Peer 1 must still receive its full 1428-byte cap below.
                extra = (struct.pack('>BBH', 2, 0, 1280) + socket.inet_aton('172.31.250.13') +
                         struct.pack('>H', 51888) + D.key(48) + bytes([1]) +
                         D.pfx('10.77.3.0', 24) + b'\0' * 15)
                snap += D.G.attr(2, extra)
                target_mtu = 1208

            def committed():
                result = D.sh('ip', '-j', 'link', 'show', 'vpn0', ns=gw.site, check_rc=True)
                self.check(f'{gw.site}-minimum-mtu-before-key-install',
                           json.loads(result.stdout)[0]['mtu'] == target_mtu)

            D.prepare(gw, snap, after_commit=committed)
            keys[gw.site] = (tx, rx)
            for mtu in (1000, 1428 if target_mtu != 1428 else 1208):
                result = D.sh('ip', 'link', 'set', 'vpn0', 'mtu', str(mtu), ns=gw.site)
                self.check(f'{gw.site}-manual-mtu-{mtu}-refused', result.returncode != 0)
        D.GWB_RX[0] = gb.local_rx
        D.finish(ga, *keys['gwa'], gb.local_rx)
        D.finish(gb, *keys['gwb'], ga.local_rx)
        D.wait_active(ga, gb)
        self.check('netlink-provisioning', D.failed == 0)
        time.sleep(0.5)
        with D.netns('hb'):
            self.receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sockets.append(self.receiver)
            self.receiver.bind((DST, 9090))
            self.receiver.setsockopt(socket.IPPROTO_IP, socket.IP_RECVTOS, 1)
            self.receiver.setsockopt(socket.IPPROTO_IP, 12, 1)  # Linux IP_RECVTTL
            self.receiver.settimeout(0.3)
        with D.netns('ha'):
            self.sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sockets.append(self.sender)
            self.sender.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xa2)
            self.sender.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, 64)
            self.sender.setsockopt(socket.IPPROTO_IP, 10, 2)  # IP_MTU_DISCOVER, IP_PMTUDISC_DO

        underlay = str(self.directory / f'outer-{self.pmtu}.pcap')
        with D.capture('und', 'veth_und_a', underlay):
            self.health('healthy-before')
            for ecn in range(4):
                payload = self.payload()
                self.sender.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xa0 | ecn)
                self.sender.sendto(payload, (DST, 9090))
                self.check(f'normal-ecn-{ecn}-source-dscp-two-hops', self.receive(payload, 0xa0 | ecn))
            self.sender.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xa2)
            # M+1 with DF is rejected by Linux before ndo_start_xmit at the
            # committed device MTU; it is not a tunnel admission drop.
            for n in (self.pmtu - 73, self.pmtu - 72, self.pmtu - 71):
                payload = self.payload(n - 28)
                before = counters(ga)
                self.sender.sendto(payload, (DST, 9090))
                allowed = n <= self.pmtu - 72
                self.check(f'tx-size-{n}', self.receive(payload if allowed else None,
                                                       0xa2 if allowed else None))
                after = counters(ga)
                self.check(f'tx-size-{n}-counter', after['TX_PACKETS'] - before['TX_PACKETS'] == int(allowed)
                           and after['TX_DROPPED'] == before['TX_DROPPED'])
        outer_packets = [p[14:] for p in D.read_pcap(underlay)
                         if len(p) >= 42 and p[12:14] == b'\x08\0' and p[23] == 17
                         and p[26:30] == socket.inet_aton(OA)
                         and struct.unpack_from('>H', p, 36)[0] == 51888]
        self.check('captured-normal-data', len(outer_packets) >= 5)
        self.check('outer-dscp-zero-ecn-all-four', {p[1] for p in outer_packets} == {0, 1, 2, 3})
        self.check('outer-df-valid-ip-udp-length-checksums-cap', bool(outer_packets) and all(
            len(p) == struct.unpack_from('>H', p, 2)[0] <= self.pmtu and
            struct.unpack_from('>H', p, 6)[0] == 0x4000 and D.csum(p[:20]) == 0 and
            len(p) - 20 == struct.unpack_from('>H', p, 24)[0] and
            D.csum(p[12:20] + struct.pack('>BBH', 0, 17, len(p) - 20) + p[20:]) == 0
            for p in outer_packets))
        expected_lengths = {28 + 40 + 72, self.pmtu - 1, self.pmtu}
        self.check('wire-expansion-72', expected_lengths <= {len(p) for p in outer_packets})
        self.observations['outer_packets'] = [dict(length=len(p), tos=p[1],
            df=bool(struct.unpack_from('>H', p, 6)[0] & 0x4000)) for p in outer_packets]

        for i in range(4):
            for o in range(4):
                payload = self.payload()
                before = counters(gb)
                self.inject(inner(payload, 0xa0 | i), 0x54 | o)
                expected = ECN[i][o]
                self.check(f'decap-ecn-{i}-{o}', self.receive(payload if expected is not None else None,
                           (0xa0 | expected) if expected is not None else None))
                after = counters(gb)
                self.check(f'decap-ecn-{i}-{o}-counters',
                           after['RX_PACKETS'] - before['RX_PACKETS'] == int(expected is not None) and
                           after['RX_ECN_ANOMALY'] - before['RX_ECN_ANOMALY'] == int(i == 0 and o != 0) and
                           after['RX_DROPPED'] - before['RX_DROPPED'] == int(expected is None))
        self.health('healthy-after-ecn')

        for n in (self.pmtu - 73, self.pmtu - 72, self.pmtu - 71):
            for df in (0, 0x4000):
                payload = self.payload(n - 28)
                before = counters(gb)
                self.inject(inner(payload, flags=df), 2)
                allowed = n <= self.pmtu - 72
                self.check(f'rx-size-{n}-df-{bool(df)}',
                           self.receive(payload if allowed else None, 0xa2 if allowed else None))
                after = counters(gb)
                self.check(f'rx-size-{n}-df-{bool(df)}-counter',
                           after['RX_PACKETS'] - before['RX_PACKETS'] == int(allowed) and
                           (allowed or after['RX_POLICY'] > before['RX_POLICY'] or
                            after['RX_MALFORMED'] > before['RX_MALFORMED']))
        self.health('healthy-after-size')

        bad = []
        p = bytearray(inner(self.payload()))
        p[-1] ^= 1
        bad.append(('udp-checksum', bytes(p)))
        p = bytearray(inner(self.payload()))
        p[25] -= 1
        bad.append(('udp-length', bytes(p)))
        p = bytearray(inner(self.payload()))
        p[10] ^= 1
        bad.append(('ip-checksum', bytes(p)))
        bad.append(('reserved-flag', inner(self.payload(), flags=0x8000)))
        # Distinct identifications: fragments of one datagram would join the same normalization set.
        udp = D.raw_udp(SRC, DST, self.payload(), 23456, 9090)[20:]
        bad.append(('inner-first-fragment', ip_header(SRC, DST, udp, 0xa2, 0x2000, ident=0x7301)))
        bad.append(('inner-noninitial-fragment', ip_header(SRC, DST, udp, 0xa2, 1, ident=0x7302)))
        original = inner(self.payload())
        bad.append(('inner-options', ip_header(SRC, DST, original[20:], options=b'\x01' * 4)))
        # Valid policy tuple but malformed TCP doff/checksum, and ICMP echo checksum.
        tcp = struct.pack('>HHIIBBHHH', 23456, 9090, 1, 0, 0x40, 2, 1000, 0, 0)
        bad.append(('tcp-doff', ip_header(SRC, DST, tcp, proto=6)))
        bad.append(('icmp-checksum', ip_header(SRC, DST, b'\x08\0\0\0\x01\x02\x03\x04', proto=1)))
        lan = str(self.directory / f'denied-{self.pmtu}.pcap')
        with D.capture('gwb', 'veth_gwb_lan', lan):
            for label, packet in bad:
                before = counters(gb)
                self.inject(packet)
                self.check(label + '-absent-at-listener', self.receive())
                after = counters(gb)
                if 'fragment' in label:
                    # A fragment is normalized, not dropped at intake: queued (or refused by the
                    # fragment checks), never delivered. An incomplete set expires on its own.
                    self.check(label + '-held-or-refused-never-delivered',
                               (after['FRAG_QUEUED'] - before['FRAG_QUEUED']) +
                               (after['FRAG_DROPPED'] - before['FRAG_DROPPED']) == 1 and
                               after['RX_PACKETS'] == before['RX_PACKETS'])
                else:
                    self.check(label + '-rejected-before-delivery',
                               after['RX_DROPPED'] - before['RX_DROPPED'] == 1 and
                               after['RX_PACKETS'] == before['RX_PACKETS'])
        self.check('malformed-never-reached-lan', not [p for p in D.read_pcap(lan)
                   if len(p) >= 34 and p[12:14] == b'\x08\0'])
        # A legal zero UDP checksum is still accepted after strict extent checks.
        payload = self.payload()
        p = bytearray(inner(payload))
        p[26:28] = b'\0\0'
        self.inject(bytes(p), 2)
        self.check('ipv4-zero-inner-udp-checksum-legal', self.receive(payload, 0xa2))
        self.health('healthy-after-malformed')

        for kind in ('zero-udp', 'bad-udp', 'bad-ip'):
            self.counter += 1
            carrier = bytearray(D.raw_udp(OA, OB, protect(inner(self.payload()), self.counter)))
            before = counters(gb)
            before_checksum = snmp_counter('Ip', 'InHdrErrors') if kind == 'bad-ip' else snmp_counter('Udp', 'InCsumErrors')
            if kind == 'zero-udp':
                carrier[26:28] = b'\0\0'
            elif kind == 'bad-udp':
                carrier[-1] ^= 1
            else:
                carrier[10] ^= 1
            ethernet_send(bytes(carrier))
            self.check('outer-' + kind + '-absent', self.receive())
            after = counters(gb)
            if kind == 'zero-udp':
                rejected = after['RX_MALFORMED'] - before['RX_MALFORMED'] == 1
            else:
                rejected = ((snmp_counter('Ip', 'InHdrErrors') if kind == 'bad-ip' else
                             snmp_counter('Udp', 'InCsumErrors')) - before_checksum == 1)
            self.check('outer-' + kind + '-counted', rejected and after['RX_PACKETS'] == before['RX_PACKETS'])
        self.health('healthy-after-outer-checksums')

        # Enable Linux shared defrag deliberately; counter control below proves
        # the same fragmented carrier can reassemble at another local address.
        D.sh('nft', 'add', 'table', 'ip', 'hr4ct', ns='gwb', check_rc=True)
        D.sh('nft', 'add', 'chain', 'ip', 'hr4ct', 'input',
             '{ type filter hook input priority 0; policy accept; }', ns='gwb', check_rc=True)
        D.sh('nft', 'add', 'rule', 'ip', 'hr4ct', 'input', 'ct', 'state', 'established', 'accept',
             ns='gwb', check_rc=True)
        before, reasm = counters(gb), reassembly_requests()
        udp = D.raw_udp(OA, OB, protect(inner(self.payload()), self.counter + 1))[20:]
        self.counter += 1
        for chunk, flags in ((udp[:24], 0x2000), (udp[24:], 3)):
            ethernet_send(ip_header(OA, OB, chunk, flags=flags))
        self.check('outer-fragments-not-delivered', self.receive())
        after = counters(gb)
        self.check('outer-fragments-counted-before-shared-defrag',
                   after['RX_MALFORMED'] - before['RX_MALFORMED'] == 2 and
                   reassembly_requests() == reasm and after['RX_PACKETS'] == before['RX_PACKETS'])
        self.observations['outer_fragment_guard'] = dict(
            malformed_delta=after['RX_MALFORMED'] - before['RX_MALFORMED'],
            reassembly_requests_before=reasm, reassembly_requests_after=reassembly_requests())
        before = counters(gb)
        self.inject(inner(self.payload()), options=b'\x01' * 4)
        self.check('outer-options-not-delivered', self.receive())
        self.check('outer-options-counted', counters(gb)['RX_MALFORMED'] - before['RX_MALFORMED'] == 1)
        alias = '172.31.250.112'
        D.sh('ip', 'addr', 'add', alias + '/24', 'dev', 'veth_gwb_out', ns='gwb', check_rc=True)
        with D.netns('gwb'):
            other = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sockets.append(other)
            other.bind((alias, 9091))
            other.settimeout(1)
        payload = self.payload(80)
        udp = D.raw_udp(OA, alias, payload, dport=9091)[20:]
        for chunk, flags in ((udp[:24], 0x2000), (udp[24:], 3)):
            ethernet_send(ip_header(OA, alias, chunk, flags=flags, ident=0x7374))
        data = other.recv(1000)
        self.check('unrelated-local-alias-reassembles', data == payload and reassembly_requests() > reasm)
        with D.netns('gwb'):
            local = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sockets.append(local)
            local.bind((OB, 9091))
            local.settimeout(1)
        ethernet_send(D.raw_udp(OA, OB, payload, dport=9091))
        self.check('unfragmented-unrelated-port-preserved', local.recv(1000) == payload)
        self.health('healthy-after-outer-guard')
        self.observations['final_counters_a'] = counters(ga)
        self.observations['final_counters_b'] = counters(gb)

    def run(self):
        setup = subprocess.run(['bash', str(Path(D.HERE) / 'n1_fixture.sh'), 'setup', D.RUN],
                               capture_output=True, text=True, timeout=60)
        if setup.returncode:
            raise RuntimeError(setup.stderr or setup.stdout)
        try:
            self.exercise()
        finally:
            for gw in self.gateways:
                gw.stop.set()
                if gw.thread:
                    gw.thread.join(timeout=3)
                gw.owner.close()
                gw.observer.close()
            for sock in self.sockets:
                sock.close()
            clean = subprocess.run(['bash', str(Path(D.HERE) / 'n1_fixture.sh'), 'teardown', D.RUN],
                                   capture_output=True, text=True, timeout=60)
            self.check('fixture-teardown', clean.returncode == 0, clean.stderr)
            remaining = subprocess.check_output(['ip', 'netns', 'list'], text=True)
            self.check('no-fixture-namespaces', f'vpt_{D.RUN}_' not in remaining)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or os.environ.get('VPT1_DISPOSABLE_VM') != 'yes':
        parser.error('requires root and VPT1_DISPOSABLE_VM=yes')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    before = root_network()
    report = dict(status='ERROR', cases=[], observations={}, scope='thin unfragmented IPv4 path',
                  kernel=os.uname().release, page_size=os.sysconf('SC_PAGE_SIZE'))
    report['pmtu_scope'] = ('Configured P=1500/1280, committed shared TX minimum, peer-specific RX cap. '
                            'No automatic path-MTU discovery, output-fragment or associated-error qualification.')
    report['underlay_gro_scope'] = 'Capture aggregation disabled; GRO/fraglist receive unqualified.'
    directory = args.output.parent / (args.output.stem + '-packets')
    directory.mkdir(exist_ok=True)
    try:
        for pmtu in (1500, 1280):
            campaign = Campaign(pmtu, directory)
            try:
                campaign.run()
            finally:
                report['cases'].extend(dict(pmtu=pmtu, **case) for case in campaign.results)
                report['observations'][str(pmtu)] = campaign.observations
        report['root_network_unchanged'] = before == root_network()
        report['status'] = ('PASS' if report['root_network_unchanged'] and
                            all(c['passed'] for c in report['cases']) else 'FAIL')
    except Exception as exc:
        report['error'] = str(exc)
    report['packet_capture_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                       for p in sorted(directory.glob('*.pcap'))}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HR4_IP {report['status']} cases={len(report['cases'])}", flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
