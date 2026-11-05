#!/usr/bin/env python3
"""HR3/HR4 live IP adapter on a real kernel: fragments, associated ICMP errors, bounded GSO.

Root, disposable 6.18 VM, vpt1_foundation loaded. Uses the N1 fixture (two sites joined only by an
underlay bridge), real Netlink provisioning and the independent OpenSSL record oracle of hr4_ip.py.

  * round trips of UDP datagrams from 1 to 60000 bytes through both gateways, each request and
    reply normalized, policy-checked once and delivered once, byte for byte; the underlay carries only
    DF-set UDP/51888 records no larger than the pair PMTU;
  * hostile LAN-side fragments (duplicate, overlap, hole, no first fragment, denied port, spoofed
    source) are held or refused and never delivered, each followed by a healthy control;
  * hostile tunnel-side fragments built with independent crypto (reordered with an outer CE mark,
    overlap, foreign source, non-LAN destination, DF, odd MF length, no first fragment);
  * TTL expiry exactly two forwarding hops away and a closed port come back as ICMP errors through
    the tunnel; forged or unsolicited errors are denied and counted; the error rate is bounded;
  * a 100-segment UDP GSO aggregate sends 64 packets and drops the rest, a 40-segment one all.

Counters are checked from the kernel's own COUNTERS reply, deliveries from the receiving socket.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_datapath as D  # noqa: E402
import hr4_ip as H  # noqa: E402

D.C.update(RX_ECN_ANOMALY=35, FRAG_QUEUED=50, FRAG_COMPLETE=51, FRAG_DROPPED=52, FRAG_BUSY=53,
           ERROR_ADMITTED=54, ERROR_DENIED=55, TX_GSO_LIMIT=56)

SRC, DST = '10.77.1.10', '10.77.2.10'
GWA_LAN, GWB_LAN = '10.77.1.1', '10.77.2.1'
OA, OB = '172.31.250.11', '172.31.250.12'
UDP, ICMP = 17, 1
IP_RECVERR, IP_MTU_DISCOVER, UDP_SEGMENT = 11, 10, 103

check = D.check


def delta(after, before):
    return {k: after[k] - before[k] for k in after}


def tcp_segment(src, dst, sport, dport, flags, ident):
    tcp = struct.pack('>HHIIBBHHH', sport, dport, 1000, 2000, 0x50, flags, 8192, 0, 0)
    pseudo = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack('>BBH', 0, 6, len(tcp))
    tcp = tcp[:16] + struct.pack('>H', D.csum(pseudo + tcp)) + tcp[18:]
    header = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), ident, 0x4000, 64, 6, 0,
                         socket.inet_aton(src), socket.inet_aton(dst))
    return header[:10] + struct.pack('>H', D.csum(header)) + header[12:] + tcp


def udp_datagram(src, dst, payload, sport, dport):
    length = 8 + len(payload)
    pseudo = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack('>BBH', 0, UDP, length)
    c = D.csum(pseudo + struct.pack('>HHHH', sport, dport, length, 0) + payload) or 0xFFFF
    return struct.pack('>HHHH', sport, dport, length, c) + payload


def fragment(src, dst, body, offset, size, more, ident, proto=UDP, ttl=64, tos=0, extra=0):
    """One IPv4 fragment of @body: header with real checksum; offset and size in bytes."""
    chunk = body[offset:offset + size]
    flags = (offset // 8) | (0x2000 if more else 0) | extra
    header = struct.pack('>BBHHHBBH4s4s', 0x45, tos, 20 + len(chunk), ident, flags, ttl, proto, 0,
                         socket.inet_aton(src), socket.inet_aton(dst))
    return header[:10] + struct.pack('>H', D.csum(header)) + header[12:] + chunk


def cuts(body, size):
    """Cut offsets for fragments of @size bytes (a multiple of 8) covering @body."""
    out, off = [], 0
    while off < len(body):
        n = min(size, len(body) - off)
        out.append((off, n, off + n < len(body)))
        off += n
    return out


def raw_send(site, packet, dst):
    with D.netns(site):
        s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)
        s.sendto(packet, (dst, 0))
        s.close()


class Sink:
    """A UDP socket inside a namespace that records what it receives and optionally echoes."""

    def __init__(self, site, addr, port, echo):
        with D.netns(site):
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
            # Replies are DF clear like the client's requests: a DF-set oversize reply is the PMTU case below.
            self.sock.setsockopt(socket.IPPROTO_IP, IP_MTU_DISCOVER, 0)
            self.sock.bind((addr, port))
            self.sock.settimeout(0.2)
        self.got, self.echo, self.stop = [], echo, threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def loop(self):
        while not self.stop.is_set():
            try:
                data, peer = self.sock.recvfrom(70000)
            except socket.timeout:
                continue
            except OSError:
                return
            self.got.append(data)
            if self.echo:
                try:
                    self.sock.sendto(data, peer)
                except OSError:
                    pass

    def close(self):
        self.stop.set()
        self.thread.join(2)
        self.sock.close()


def client(port=0, tos=0):
    with D.netns('ha'):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        s.setsockopt(socket.IPPROTO_IP, IP_MTU_DISCOVER, 0)  # IP_PMTUDISC_DONT: DF clear, may fragment
        s.bind((SRC, port))
        s.settimeout(1.5)
        return s


def pattern(label, length):
    head = f'{label}-{time.monotonic_ns()}-'.encode()
    return (head * (length // len(head) + 1))[:length]


def errqueue(sock):
    """(type, code, offender) for every ICMP error queued on @sock."""
    out = []
    while True:
        try:
            _, anc, _, _ = sock.recvmsg(2048, 1024, socket.MSG_ERRQUEUE | socket.MSG_DONTWAIT)
        except (BlockingIOError, OSError):
            return out
        for level, kind, data in anc:
            if level == socket.IPPROTO_IP and kind == IP_RECVERR and len(data) >= 24 and data[4] == 2:
                out.append((data[5], data[6], socket.inet_ntoa(data[20:24])))


def main():
    if os.geteuid() != 0:
        sys.exit('run as root in a disposable VM')
    r = subprocess.run(['bash', os.path.join(HERE, 'n1_fixture.sh'), 'setup', D.RUN],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'fixture setup failed: {r.stderr or r.stdout}')
    tmp = tempfile.mkdtemp(prefix='hr3live_')
    sinks, socks = [], []
    try:
        for site in ('gwa', 'gwb'):
            D.sh('ip', 'link', 'add', 'vpn0', 'type', 'vpt1_ay1', ns=site, check_rc=True)
            D.sh('ip', 'link', 'set', 'vpn0', 'up', ns=site, check_rc=True)
            for key in ('all', 'default', 'veth_gw%s_lan' % site[-1], 'vpn0'):
                D.sh('sysctl', '-q', '-w', f'net.ipv4.conf.{key}.rp_filter=0', ns=site)
        # The route's source is the gateway's LAN address: that is the source of the errors it generates.
        D.sh('ip', 'route', 'add', '10.77.2.0/24', 'dev', 'vpn0', 'src', GWA_LAN, ns='gwa', check_rc=True)
        D.sh('ip', 'route', 'add', '10.77.1.0/24', 'dev', 'vpn0', 'src', GWB_LAN, ns='gwb', check_rc=True)
        D.sh('sysctl', '-q', '-w', 'net.ipv4.icmp_ratelimit=0', ns='hb', check_rc=True)
        D.sh('sysctl', '-q', '-w', 'net.ipv4.icmp_ratelimit=0', ns='gwb', check_rc=True)
        D.sh('sysctl', '-q', '-w', 'net.ipv4.icmp_ratelimit=0', ns='gwa', check_rc=True)

        ga, gb = D.Gateway('gwa', D.ID_A), D.Gateway('gwb', D.ID_B)
        ports = (7000, 7002, 7003)
        rules_a = [(i + 1, 1, UDP, p, SRC, DST) for i, p in enumerate(ports)] + [(9, 1, 6, 8080, SRC, DST)]
        rules_b = [(i + 1, 2, UDP, p, SRC, DST) for i, p in enumerate(ports)] + [(9, 2, 6, 8080, SRC, DST)]
        D.prepare(ga, D.snapshot(ga.lan_if, ga.und_if, '10.77.1.0', GWA_LAN, OA, OB, '10.77.2.0',
                                 rules_a, 0x10, 0x20))
        D.prepare(gb, D.snapshot(gb.lan_if, gb.und_if, '10.77.2.0', GWB_LAN, OB, OA, '10.77.1.0',
                                 rules_b, 0x20, 0x10))
        D.GWB_RX[0] = gb.local_rx
        D.finish(ga, D.K1, D.K2, gb.local_rx)
        D.finish(gb, D.K2, D.K1, ga.local_rx)
        D.wait_active(ga, gb)
        time.sleep(0.5)

        echo = Sink('hb', DST, 7000, True)
        silent = Sink('hb', DST, 7003, False)
        sinks += [echo, silent]
        cnt = lambda g: H.counters(g)  # noqa: E731
        pcap = os.path.join(tmp, 'und.pcap')
        lan_b = os.path.join(tmp, 'lanb.pcap')

        # ---- 1. round trips of every size, both directions ------------------------------------
        with D.capture('und', 'veth_und_a', pcap):
            c = client()
            socks.append(c)
            for size in (1, 1400, 1401, 1472, 1473, 2900, 4000, 9000, 20000, 40000):
                payload = pattern(f's{size}', size)
                a0, b0 = cnt(ga), cnt(gb)
                n0 = len(echo.got)
                c.sendto(payload, (DST, 7000))
                try:
                    reply = c.recv(70000)
                except socket.timeout:
                    reply = None
                a1, b1 = cnt(ga), cnt(gb)
                da, db = delta(a1, a0), delta(b1, b0)
                check(f'size-{size}-echoed-byte-for-byte', reply == payload,
                      f'got {None if reply is None else len(reply)}')
                check(f'size-{size}-delivered-once', len(echo.got) - n0 == 1 and echo.got[-1] == payload)
                big = size > 1400
                # A request and its reply: each is normalized and completed once on each gateway.
                check(f'size-{size}-completed-twice-per-gateway',
                      (da['FRAG_COMPLETE'], db['FRAG_COMPLETE']) == ((2, 2) if big else (0, 0)),
                      f'{da} {db}')
                check(f'size-{size}-nothing-dropped-or-refused',
                      da['FRAG_DROPPED'] == da['FRAG_BUSY'] == db['FRAG_DROPPED'] == db['FRAG_BUSY'] == 0 and
                      da['TX_DROPPED'] == db['TX_DROPPED'] == 0, f'{da} {db}')
        # The cache holds 64 pieces per set. A 60000-byte datagram reaches the gateway as ~82 pieces (the host's
        # 1500-byte fragments, cut again for vpn0's 1428-byte MTU), so it is refused, counted, and harmless. This runs
        # before the DF case below: that one teaches the host the 1428-byte path MTU, after which it fragments
        # to 1408-byte pieces and a datagram can no longer reach 64 of them.
        a0 = cnt(ga)
        n0 = len(echo.got)
        c.sendto(pattern('huge', 60000), (DST, 7000))
        time.sleep(1.0)
        d = delta(cnt(ga), a0)
        # Refused either by the cache (the 65th piece) or by the bounded job queue (64 per gateway) when pieces
        # arrive faster than the worker drains them; nothing completes and nothing is delivered.
        check('over-64-pieces-refused', len(echo.got) == n0 and d['FRAG_DROPPED'] + d['FRAG_BUSY'] >= 1 and
              d['FRAG_COMPLETE'] == 0, str(d))
        # DF set and too large for the tunnel: the host's own gateway answers with fragmentation-needed
        # (this site's LAN address, MTU 1428); the datagram is not fragmented and not sent.
        d_sock = client()
        socks.append(d_sock)
        d_sock.setsockopt(socket.IPPROTO_IP, IP_MTU_DISCOVER, 2)  # IP_PMTUDISC_DO
        d_sock.setsockopt(socket.IPPROTO_IP, IP_RECVERR, 1)
        a0, n0 = cnt(ga), len(echo.got)
        d_sock.sendto(pattern('df', 1472), (DST, 7000))
        time.sleep(0.6)
        errs = []
        while True:
            try:
                _, anc, _, _ = d_sock.recvmsg(2048, 1024, socket.MSG_ERRQUEUE | socket.MSG_DONTWAIT)
            except (BlockingIOError, OSError):
                break
            for level, kind, data in anc:
                if level == socket.IPPROTO_IP and kind == IP_RECVERR and data[4] == 2:
                    errs.append((data[5], data[6], socket.inet_ntoa(data[20:24]), struct.unpack('=I', data[8:12])[0]))
        d = delta(cnt(ga), a0)
        check('df-oversize-gets-fragmentation-needed-from-its-gateway', errs == [(3, 4, GWA_LAN, 1428)], f'{errs}')
        check('df-oversize-never-sent', len(echo.got) == n0 and d['TX_PACKETS'] == 0 and d['FRAG_QUEUED'] == 0, str(d))
        pk = D.read_pcap(pcap)
        outer = [p[14:] for p in pk if len(p) > 42 and p[12:14] == b'\x08\x00' and p[14 + 9] == UDP]
        check('underlay-carries-records', len(outer) >= 100, f'{len(outer)}')
        check('records-df-set-and-within-pmtu', all(
            struct.unpack_from('>H', p, 6)[0] == 0x4000 and struct.unpack_from('>H', p, 2)[0] <= 1500
            for p in outer))
        check('no-private-ipv4-on-the-underlay', not [p for p in pk if len(p) > 34 and
                                                      p[12:14] == b'\x08\x00' and p[26:28] == b'\x0a\x4d'])

        # ---- 2. hostile LAN-side fragments ----------------------------------------------------
        def healthy(label):
            payload = pattern(label, 3000)
            n0 = len(echo.got)
            c.sendto(payload, (DST, 7000))
            try:
                ok = c.recv(70000) == payload
            except socket.timeout:
                ok = False
            check(label, ok and len(echo.got) - n0 == 1)

        port = c.getsockname()[1]
        ident = [0x5000]

        def send_set(parts, body, src=SRC, dst=DST, order=None, extra=0, ttl=64, tos=0):
            ident[0] += 1
            out = [fragment(src, dst, body, o, n, m, ident[0], ttl=ttl, tos=tos, extra=extra)
                   for o, n, m in parts]
            for i in (order if order is not None else range(len(out))):
                raw_send('ha', out[i], dst)
            return ident[0]

        def hostile(label, action, queued=None, dropped=None, delivered=False, wait=0.6):
            a0, b0, n0 = cnt(ga), cnt(gb), len(echo.got) + len(silent.got)
            with D.capture('gwb', 'veth_gwb_lan', lan_b):
                action()
                time.sleep(wait)
            a1, b1 = cnt(ga), cnt(gb)
            d = delta(a1, a0)
            got = len(echo.got) + len(silent.got) - n0
            check(label + '-delivery', (got == 1) == delivered and got <= 1, f'delivered {got}')
            if queued is not None:
                check(label + '-queued', d['FRAG_QUEUED'] == queued, str(d))
            if dropped is not None:
                check(label + '-refused', d['FRAG_DROPPED'] == dropped, str(d))
            if not delivered:
                check(label + '-complete-never', d['FRAG_COMPLETE'] == 0 and delta(b1, b0)['FRAG_COMPLETE'] == 0)
                lan = [p for p in D.read_pcap(lan_b) if len(p) > 42 and p[12:14] == b'\x08\x00' and
                       p[26:30] == socket.inet_aton(SRC)]
                check(label + '-not-on-the-far-lan', not lan, f'{len(lan)} frames')

        body = udp_datagram(SRC, DST, pattern('hostile', 2992), port, 7003)
        three = cuts(body, 1000)
        hostile('reordered-set', lambda: send_set(three, body, order=[2, 0, 1]),
                queued=3, dropped=0, delivered=True)
        healthy('healthy-after-reorder')
        hostile('exact-duplicate', lambda: send_set(three, body, order=[0, 1, 1, 2]),
                dropped=1)  # any overlap rejects the whole set, even an identical retransmission
        healthy('healthy-after-duplicate')
        overlap = [(0, 1000, True), (800, 1000, True), (1800, len(body) - 1800, False)]
        hostile('overlap', lambda: send_set(overlap, body), dropped=1)
        healthy('healthy-after-overlap')
        hostile('hole-then-expiry', lambda: send_set(three, body, order=[0, 2]), queued=2, dropped=0)
        time.sleep(6.5)  # the set is gone; the missing fragment alone cannot complete it
        hostile('late-fragment-after-expiry', lambda: raw_send(
            'ha', fragment(SRC, DST, body, 1000, 1000, True, ident[0]), DST), queued=1)
        healthy('healthy-after-expiry')
        hostile('no-first-fragment', lambda: send_set(three[1:], body), queued=2)
        healthy('healthy-after-no-first')
        denied = udp_datagram(SRC, DST, pattern('denied', 2992), port, 7001)
        hostile('denied-port-set', lambda: send_set(cuts(denied, 1000), denied),
                queued=3, dropped=1)
        healthy('healthy-after-denied-set')
        other = udp_datagram('10.77.1.77', DST, pattern('lanspoof', 2992), port, 7003)
        hostile('lan-source-without-rule', lambda: send_set(cuts(other, 1000), other, src='10.77.1.77'),
                queued=3, dropped=1)
        healthy('healthy-after-lan-spoof')
        far = udp_datagram('10.99.1.1', DST, pattern('farspoof', 2992), port, 7003)
        hostile('source-outside-the-lan', lambda: send_set(cuts(far, 1000), far, src='10.99.1.1'),
                queued=0, dropped=3)
        healthy('healthy-after-far-spoof')

        # ---- 3. hostile tunnel-side fragments, independent crypto -------------------------------
        base = cnt(ga)['TX_PACKETS'] + 600
        H.Campaign.counter = base

        def tunnel_set(parts, body, src=SRC, dst=DST, tos=0, extra=0, outer=None, order=None):
            ident[0] += 1
            frames = [fragment(src, dst, body, o, n, m, ident[0], tos=tos, extra=extra)
                      for o, n, m in parts]
            for i in (order if order is not None else range(len(frames))):
                H.Campaign.counter += 1
                protected = H.protect(frames[i], H.Campaign.counter)
                udp = D.raw_udp(OA, OB, protected)[20:]
                H.ethernet_send(H.ip_header(OA, OB, udp, (outer or {}).get(i, 0)))

        def tunnel_case(label, action, queued, dropped, delivered):
            b0, n0 = cnt(gb), len(silent.got)
            action()
            time.sleep(0.7)
            d = delta(cnt(gb), b0)
            got = len(silent.got) - n0
            check(label + '-delivery', (got == 1) == delivered, f'delivered {got}')
            check(label + '-queued', d['FRAG_QUEUED'] == queued, str(d))
            check(label + '-refused', d['FRAG_DROPPED'] == dropped, str(d))
            check(label + '-complete-count', d['FRAG_COMPLETE'] == int(delivered), str(d))

        tbody = udp_datagram(SRC, DST, pattern('tunnel', 2992), 4242, 7003)
        t3 = cuts(tbody, 1000)
        tunnel_case('tunnel-reordered-set', lambda: tunnel_set(t3, tbody, order=[2, 1, 0]), 3, 0, True)
        check('tunnel-set-delivered-intact', silent.got and silent.got[-1] == tbody[8:])
        tunnel_case('tunnel-ecn-merge-ect-and-ce', lambda: tunnel_set(
            t3, tbody, tos=2, outer={1: 3}), 3, 0, True)
        tunnel_case('tunnel-not-ect-with-ce-dropped', lambda: tunnel_set(
            t3, tbody, tos=0, outer={1: 3}), 2, 1, False)  # refused at intake, so never queued
        tunnel_case('tunnel-overlap', lambda: tunnel_set(
            [(0, 1000, True), (800, 1000, True), (1800, len(tbody) - 1800, False)], tbody), 3, 1, False)
        tunnel_case('tunnel-exact-duplicate', lambda: tunnel_set(t3, tbody, order=[0, 0, 1, 2]), 4, 1, False)
        tunnel_case('tunnel-foreign-source', lambda: tunnel_set(
            t3, tbody, src='10.77.3.5'), 0, 3, False)
        tunnel_case('tunnel-non-lan-destination', lambda: tunnel_set(
            t3, tbody, dst='10.77.9.9'), 0, 3, False)
        tunnel_case('tunnel-df-fragment', lambda: tunnel_set(t3, tbody, extra=0x4000), 0, 3, False)
        tunnel_case('tunnel-odd-mf-length', lambda: tunnel_set(cuts(tbody, 1001), tbody), 1, 2, False)
        tunnel_case('tunnel-no-first-fragment', lambda: tunnel_set(t3[1:], tbody), 2, 0, False)
        tunnel_case('tunnel-denied-port', lambda: tunnel_set(
            cuts(udp_datagram(SRC, DST, pattern('tdenied', 2992), 4242, 7001), 1000),
            udp_datagram(SRC, DST, pattern('tdenied', 2992), 4242, 7001)), 3, 1, False)
        healthy('healthy-after-tunnel-hostile')

        # ---- 4. associated ICMP errors through the tunnel --------------------------------------
        e = client()
        socks.append(e)
        e.setsockopt(socket.IPPROTO_IP, IP_RECVERR, 1)
        e.settimeout(0.5)
        a0, b0 = cnt(ga), cnt(gb)
        for ttl, expect in ((1, (11, 0, GWA_LAN)), (2, (11, 0, GWB_LAN))):
            e.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, ttl)
            e.sendto(pattern(f'ttl{ttl}', 64), (DST, 7003))
            time.sleep(0.6)
            errs = errqueue(e)
            check(f'ttl-{ttl}-expires-at-hop-{ttl}', errs == [expect], f'{errs}')
        e.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, 3)
        n0 = len(silent.got)
        e.sendto(pattern('ttl3', 64), (DST, 7003))
        time.sleep(0.6)
        check('ttl-3-reaches-the-host-after-exactly-two-gateways', len(silent.got) - n0 == 1 and not errqueue(e))
        a1, b1 = cnt(ga), cnt(gb)
        check('remote-ttl-error-counted-on-both-gateways',
              b1['ERROR_ADMITTED'] - b0['ERROR_ADMITTED'] == 1 and
              a1['ERROR_ADMITTED'] - a0['ERROR_ADMITTED'] == 1, f'{delta(a1, a0)} {delta(b1, b0)}')
        e.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, 64)

        # A closed port on the allowed service: the initial-flow error returns.
        a0, b0 = cnt(ga), cnt(gb)
        e.sendto(pattern('closed', 64), (DST, 7002))
        time.sleep(0.6)
        errs = errqueue(e)
        check('closed-port-unreachable-returns', errs == [(3, 3, DST)], f'{errs}')
        a1, b1 = cnt(ga), cnt(gb)
        check('closed-port-error-counted', b1['ERROR_ADMITTED'] - b0['ERROR_ADMITTED'] == 1 and
              a1['ERROR_ADMITTED'] - a0['ERROR_ADMITTED'] == 1 and
              b1['ERROR_DENIED'] == b0['ERROR_DENIED'], f'{delta(a1, a0)} {delta(b1, b0)}')

        # Forged errors from the far LAN: nothing was sent that they could answer.
        def forged(src, quote_dport, quote_src=SRC):
            quoted = udp_datagram(quote_src, DST, b'x' * 4, 43210, quote_dport)
            quoted = fragment(quote_src, DST, quoted, 0, len(quoted), False, 0x7777)
            msg = struct.pack('>BBHI', 3, 3, 0, 0) + quoted[:28]
            msg = msg[:2] + struct.pack('>H', D.csum(msg)) + msg[4:]
            hdr = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(msg), 0x9999, 0, 64, ICMP, 0,
                              socket.inet_aton(src), socket.inet_aton(quote_src))
            return hdr[:10] + struct.pack('>H', D.csum(hdr)) + hdr[12:] + msg

        lan_a = os.path.join(tmp, 'lana.pcap')
        b0 = cnt(gb)
        with D.capture('ha', 'veth_ha', lan_a):
            raw_send('hb', forged(DST, 7000), SRC)          # an error for a packet never sent
            raw_send('hb', forged(DST, 7002, '10.77.1.11'), '10.77.1.11')  # a different local host
            raw_send('hb', forged('10.88.0.1', 7002), SRC)  # a foreign source address
            time.sleep(0.7)
        d = delta(cnt(gb), b0)
        check('forged-errors-denied-and-counted', d['ERROR_DENIED'] >= 2 and d['ERROR_ADMITTED'] == 0, str(d))
        icmp = [p for p in D.read_pcap(lan_a) if len(p) > 34 and p[12:14] == b'\x08\x00' and p[23] == ICMP and
                p[34] == 3]
        check('forged-errors-never-reach-the-lan', not icmp, f'{len(icmp)} frames')
        check('forged-errors-leave-no-socket-error', errqueue(e) == [])

        # Rate: a burst of closed-port probes cannot turn into an unbounded stream of errors.
        a0, b0 = cnt(ga), cnt(gb)
        for i in range(60):
            try:
                e.sendto(pattern(f'burst{i}', 32), (DST, 7002))
            except OSError:
                pass  # the previous ICMP error is reported on the next call; it is also on the error queue
        time.sleep(1.0)
        d = delta(cnt(gb), b0)
        errs = len(errqueue(e))
        check('error-rate-is-bounded', 15 <= d['ERROR_ADMITTED'] <= 30 and 15 <= errs <= d['ERROR_ADMITTED'] and
              d['ERROR_DENIED'] >= 10, f'{d} delivered {errs}')
        healthy('healthy-after-errors')

        # ---- 4b. strict initiation: the service host cannot start a connection from its service port ------
        lan_t = os.path.join(tmp, 'lant.pcap')
        b0 = cnt(gb)
        with D.capture('ha', 'veth_ha', lan_t):
            raw_send('hb', tcp_segment(DST, SRC, 8080, 45000, 0x02, 0x6001), SRC)  # a bare SYN
            time.sleep(0.5)
            raw_send('hb', tcp_segment(DST, SRC, 8080, 45000, 0x12, 0x6002), SRC)  # SYN-ACK
            raw_send('hb', tcp_segment(DST, SRC, 8080, 45000, 0x10, 0x6003), SRC)  # ACK
            time.sleep(0.6)
        flags = sorted(p[47] for p in D.read_pcap(lan_t) if len(p) > 47 and p[12:14] == b'\x08\x00' and
                       p[23] == 6 and p[26:30] == socket.inet_aton(DST))
        check('bare-syn-from-the-service-port-never-reaches-the-lan', 0x02 not in flags, f'{flags}')
        check('syn-ack-and-ack-take-the-reply-path', flags == [0x10, 0x12], f'{flags}')
        check('bare-syn-counted-as-dropped-by-its-gateway', delta(cnt(gb), b0)['TX_DROPPED'] == 1)
        healthy('healthy-after-reply-syn')

        # ---- 5. bounded GSO ---------------------------------------------------------------------
        g = client()
        socks.append(g)
        g.setsockopt(socket.IPPROTO_UDP, UDP_SEGMENT, 200)
        for segments, expect_sent in ((40, 40), (100, 64)):
            a0 = cnt(ga)
            n0 = len(silent.got)
            g.sendto(b''.join(bytes([i % 251 + 1]) * 200 for i in range(segments)), (DST, 7003))
            time.sleep(1.0)
            d = delta(cnt(ga), a0)
            got = silent.got[n0:]
            check(f'gso-{segments}-segments-delivered-{expect_sent}', len(got) == expect_sent,
                  f'delivered {len(got)} {d}')
            check(f'gso-{segments}-excess-counted', d['TX_GSO_LIMIT'] == segments - expect_sent, str(d))
            check(f'gso-{segments}-first-segments-intact', all(
                x == bytes([i % 251 + 1]) * 200 for i, x in enumerate(got[:expect_sent])))
        healthy('healthy-after-gso')

        # ---- 5b. a re-key: fragments are admitted under the generation they arrived under --------------
        # New keys, new locators: the current TX and RX generations are no longer the ones used so far.
        before = {g.site: g.counters()['ACTIVATED'] for g in (ga, gb)}
        att = {g.site: g.begin_attempt() for g in (ga, gb)}
        k_a = bytes(range(60, 92))
        k_b = bytes(range(120, 152))
        for gw, other, k_tx, k_rx in ((ga, gb, k_a, k_b), (gb, ga, k_b, k_a)):
            inc, attempt_id, local = att[gw.site]
            err = gw.cmd('STAGE_GENERATION', D.G.a('PEER_SLOT', D.G.u16(1)),
                         D.G.a('PEER_INCARNATION', inc), D.G.a('ATTEMPT_ID', attempt_id),
                         D.G.a('HANDSHAKE_HASH', b'H' * 32), D.G.a('LOCAL_RX', D.G.u64(local)),
                         D.G.a('REMOTE_RX', D.G.u64(att[other.site][2])), D.G.a('TX_KEY', k_tx),
                         D.G.a('RX_KEY', k_rx), D.G.a('MODE', b'\0'))[0]
            check(f'rekey-{gw.site}-staged', err == 0, f'err={err}')
        D.wait_active(ga, gb, before=before)
        time.sleep(0.5)
        for size in (1472, 20000):
            payload = pattern(f'rekey{size}', size)
            a0, b0 = cnt(ga), cnt(gb)
            c.sendto(payload, (DST, 7000))
            try:
                reply = c.recv(70000)
            except socket.timeout:
                reply = None
            da, db = delta(cnt(ga), a0), delta(cnt(gb), b0)
            check(f'after-rekey-{size}-fragmented-round-trip', reply == payload, f'{da} {db}')
            check(f'after-rekey-{size}-completed-twice-per-gateway',
                  (da['FRAG_COMPLETE'], db['FRAG_COMPLETE']) == (2, 2) and da['FRAG_DROPPED'] == db['FRAG_DROPPED'] == 0,
                  f'{da} {db}')

        # ---- 6. a set held at removal ------------------------------------------------------------
        send_set(three[:2], body)
        time.sleep(0.3)
        ga.stop.set()
        time.sleep(1.2)
        check('remove-gateway-with-held-set', ga.cmd('REMOVE_GATEWAY')[0] == 0)
        check('no-traffic-after-removal', D.sh('ping', '-c', '1', '-W', '1', DST, ns='ha').returncode != 0)
    finally:
        for s in sinks:
            s.close()
        for s in socks:
            s.close()
        subprocess.run(['bash', os.path.join(HERE, 'n1_fixture.sh'), 'teardown', D.RUN], capture_output=True)
    print(f"HR3_LIVE {'PASS' if not D.failed else 'FAIL'} passed={D.passed} failed={D.failed}")
    sys.exit(1 if D.failed else 0)


if __name__ == '__main__':
    main()
