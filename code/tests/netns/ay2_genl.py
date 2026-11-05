#!/usr/bin/env python3
"""AY2 end-to-end test of the vptunnel Generic Netlink endpoint on a real kernel.

Run as root inside a disposable VM with vpt1_foundation loaded (the debug build,
make VPT1_DEBUG=1, also enables the cleanup-stall case). It unshares a private
network namespace, creates vpn0 there and drives every command through raw
AF_NETLINK sockets, so no host networking is touched.
"""
import os
import socket
import struct
import sys
import time

NETLINK_GENERIC = 16
GENL_ID_CTRL = 0x10
NLM_F_REQUEST, NLM_F_ACK = 1, 4
NLMSG_ERROR = 2
NLA_F_NESTED = 0x8000
A = dict(IFINDEX=1, CONTROLLER_ID=2, CONFIG_REVISION=3, PEER_SLOT=4,
         PEER_INCARNATION=5, ATTEMPT_ID=6, HANDSHAKE_HASH=7, LOCAL_RX=8,
         REMOTE_RX=9, TX_KEY=10, RX_KEY=11, MODE=12, PARENT_HASH=13,
         EXCHANGE_BYTES=14, SNAPSHOT=15, INTENT=16, STATUS=17, COUNTERS=18,
         REQUEST_ID=19)
C = dict(CLAIM=1, RENEW=2, VALIDATE=3, COMMIT_CONFIG=4, STAGE_GENERATION=5,
         EXCHANGE_TX=6, CONTROL_INTENT=7, DISABLE=8, REVOKE=9, STATUS=10,
         COUNTERS=11, REMOVE_GATEWAY=12, BEGIN_ATTEMPT=13)
ST = dict(OWNED=1, STOPPING=2, OWNER_EPOCH=3, LEASE_MS=4, REVISION=5,
          STAGED=6, PEER=7)
PST = dict(SLOT=1, STATE=2, HAS_GEN=3, PMTU=4, ENDPOINT=5, PORT=6, INC=7)
STATE = dict(DOWN=0, READY=1, DISABLED=2, INVALID=3)
EPERM, ENOENT, EBUSY, ENODEV, EINVAL, ESTALE, EEXIST, EOPNOTSUPP, ERANGE, ETIMEDOUT = (
    1, 2, 16, 19, 22, 116, 17, 95, 34, 110)

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def attr(kind, payload, nested=False):
    kind |= NLA_F_NESTED if nested else 0
    raw = struct.pack("=HH", 4 + len(payload), kind) + payload
    return raw + b"\0" * (-len(raw) % 4)


def u16(v): return struct.pack("=H", v)
def u32(v): return struct.pack("=I", v)
def u64(v): return struct.pack("=Q", v)


def parse_attrs(data):
    out = {}
    while len(data) >= 4:
        length, kind = struct.unpack_from("=HH", data)
        if length < 4:
            break
        out.setdefault(kind & 0x3fff, []).append(data[4:length])
        data = data[(length + 3) & ~3:]
    return out


class Genl:
    def __init__(self):
        self.s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, NETLINK_GENERIC)
        self.s.bind((0, 0))
        self.s.settimeout(5)
        self.portid = self.s.getsockname()[0]
        self.seq = 0
        self.family = GENL_ID_CTRL
        self.family = self.resolve()

    def send(self, family, cmd, attrs, version=1):
        self.seq += 1
        body = struct.pack("=BBH", cmd, version, 0) + b"".join(attrs)
        hdr = struct.pack("=IHHII", 16 + len(body), family,
                          NLM_F_REQUEST | NLM_F_ACK, self.seq, 0)
        self.s.send(hdr + body)

    def recv_all(self):
        """Returns (errno, reply attrs): errno 0 on ACK."""
        reply = {}
        while True:
            data = self.s.recv(65536)
            while len(data) >= 16:
                length, mtype, _, _, _ = struct.unpack_from("=IHHII", data)
                body = data[16:length]
                if mtype == NLMSG_ERROR:
                    return -struct.unpack_from("=i", body)[0], reply
                if mtype == self.family:
                    reply = parse_attrs(body[4:])
                data = data[(length + 3) & ~3:]

    def resolve(self):
        self.send(GENL_ID_CTRL, 3, [attr(2, b"vptunnel\0")], version=2)
        err, reply = self.recv_all()
        if err:
            raise RuntimeError(f"family vptunnel not found: {err}")
        return struct.unpack("=H", reply[1][0])[0]

    def cmd(self, name, *attrs):
        self.send(self.family, C[name], list(attrs))
        return self.recv_all()

    def close(self):
        self.s.close()


def a(name, value):
    return attr(A[name], value)


def owner(ifx, cid):
    return [a("IFINDEX", u32(ifx)), a("CONTROLLER_ID", cid)]


def pfx(addr, length):
    return socket.inet_aton(addr) + bytes([length])


UND = {"if": 3, "addr": "172.31.250.1"}  # the outer interface and address commit binds to


def snapshot(ifx, rules=(1, 2), key_peer1=0x20, endpoint1="172.31.250.12", und_addr=None):
    def key(seed): return bytes((seed + i) & 255 for i in range(32))
    local = (struct.pack(">II", ifx, UND["if"]) + pfx("10.77.1.0", 24) +
             pfx("172.31.250.0", 24) + socket.inet_aton("10.77.1.1") +
             socket.inet_aton(und_addr or UND["addr"]) + key(0x10))
    peers = []
    for slot, ep, seed, net in ((1, endpoint1, key_peer1, "10.77.2.0"),
                                (2, "172.31.250.13", 0x30, "10.77.3.0")):
        peers.append(struct.pack(">BBH", slot, 0, 1500) + socket.inet_aton(ep) +
                     struct.pack(">H", 51888) + key(seed) + bytes([1]) +
                     pfx(net, 24) + b"\0" * 15)
    out = attr(1, local) + b"".join(attr(2, p) for p in peers)
    for rid in rules:
        dst = {1: "10.77.2.10", 2: "10.77.3.10"}[rid]
        out += attr(3, struct.pack(">IBBBHH", rid, rid, 1, 6, 8080, 8080) +
                    pfx("10.77.1.10", 32) + pfx(dst, 32))
    return out


def status(g, ifx):
    err, reply = g.cmd("STATUS", a("IFINDEX", u32(ifx)))
    if err:
        return err, None
    st = parse_attrs(reply[A["STATUS"]][0])
    out = {"owned": st[ST["OWNED"]][0][0], "stopping": st[ST["STOPPING"]][0][0],
           "epoch": struct.unpack("=Q", st[ST["OWNER_EPOCH"]][0])[0],
           "lease_ms": struct.unpack("=Q", st[ST["LEASE_MS"]][0])[0],
           "revision": struct.unpack("=Q", st[ST["REVISION"]][0])[0],
           "staged": (struct.unpack("=Q", st[ST["STAGED"]][0])[0]
                      if ST["STAGED"] in st else None), "peers": {}}
    for raw in st.get(ST["PEER"], []):
        p = parse_attrs(raw)
        slot = struct.unpack("=H", p[PST["SLOT"]][0])[0]
        out["peers"][slot] = {"state": p[PST["STATE"]][0][0],
                              "gen": p[PST["HAS_GEN"]][0][0],
                              "inc": p[PST["INC"]][0]}
    return 0, out


def run_in_ns():
    os.unshare(os.CLONE_NEWNET)
    os.system("ip link set lo up")
    if os.system("ip link add vpn0 type vpt1_ay1") != 0:
        sys.exit("cannot create vpn0 (is vpt1_foundation loaded?)")
    return socket.if_nametoindex("vpn0")


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    ifx = run_in_ns()
    # Commit binds the outer UDP socket, so the snapshot's address must exist locally.
    os.system("ip link add dummy0 type dummy && ip addr add 172.31.250.1/24 dev dummy0 && ip link set dummy0 up")
    UND["if"] = socket.if_nametoindex("dummy0")
    ID1, ID2 = bytes(range(1, 17)), bytes(range(101, 117))
    g, g2 = Genl(), Genl()
    rid = a("REQUEST_ID", u64(0x1122334455667788))

    # Not claimed yet; wrong and unknown requests fail the whole request.
    check("status-before-claim", status(g, ifx)[0] == ENODEV)
    check("claim-wrong-ifindex", g.cmd("CLAIM", *owner(ifx + 50, ID1))[0] == ENODEV)
    check("claim-unknown-attribute",
          g.cmd("CLAIM", *owner(ifx, ID1), attr(20, b"\0\0\0\0"))[0] == EINVAL)
    check("claim-short-controller-id",
          g.cmd("CLAIM", a("IFINDEX", u32(ifx)), a("CONTROLLER_ID", b"x" * 8))[0] == ERANGE)
    check("claim-missing-controller-id", g.cmd("CLAIM", a("IFINDEX", u32(ifx)))[0] == EINVAL)
    check("claim-foreign-attribute",
          g.cmd("CLAIM", *owner(ifx, ID1), a("PEER_SLOT", u16(1)))[0] == EINVAL)
    check("claim-nested-flag-on-scalar",
          g.cmd("CLAIM", attr(A["IFINDEX"], u32(ifx), nested=True),
                a("CONTROLLER_ID", ID1))[0] != 0)
    check("slot-out-of-range",
          g.cmd("DISABLE", *owner(ifx, ID1), a("PEER_SLOT", u16(9)),
                a("PEER_INCARNATION", b"\0" * 16))[0] == ERANGE)

    # CAP_NET_ADMIN in the owning user namespace is required for every command.
    pid = os.fork()
    if pid == 0:
        os.setuid(65534)
        os._exit(0 if Genl().cmd("STATUS", a("IFINDEX", u32(ifx)))[0] == EPERM else 1)
    check("unprivileged-refused", os.waitpid(pid, 0)[1] == 0)

    # Claim, request-id echo, single owner, reuse of an ID is not continuity.
    err, reply = g.cmd("CLAIM", *owner(ifx, ID1), rid)
    check("claim", err == 0)
    check("request-id-echoed", reply.get(A["REQUEST_ID"], [b""])[0] == u64(0x1122334455667788))
    err, st = status(g2, ifx)
    check("status-owner", err == 0 and st["owned"] == 1 and st["epoch"] == 1 and
          0 < st["lease_ms"] <= 5000 and st["revision"] == 0)
    check("claim-other-controller-busy", g2.cmd("CLAIM", *owner(ifx, ID2))[0] == EBUSY)
    check("claim-same-id-other-portid-busy", g2.cmd("CLAIM", *owner(ifx, ID1))[0] == EBUSY)
    check("owner-gated-by-portid",
          g2.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)),
                 attr(A["SNAPSHOT"], snapshot(ifx), nested=True))[0] == EPERM)
    check("renew-stale-revision",
          g.cmd("RENEW", *owner(ifx, ID1), a("CONFIG_REVISION", u64(5)))[0] == ESTALE)
    check("renew", g.cmd("RENEW", *owner(ifx, ID1), a("CONFIG_REVISION", u64(0)))[0] == 0)

    # Snapshot validation and commit.
    bad = [("unknown-member", attr(9, b"x") + snapshot(ifx)),
           ("short-local", attr(1, b"\0" * 10)),
           ("no-local", attr(2, b"\0" * 63))]
    for name, payload in bad:
        check(f"validate-{name}",
              g.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)),
                    attr(A["SNAPSHOT"], payload, nested=True))[0] == EINVAL)
    check("validate-not-nested",
          g.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)),
                a("SNAPSHOT", snapshot(ifx)))[0] != 0)
    # An outer address that is not on this host makes the commit fail and publish nothing.
    check("validate-unbindable-address", g.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)),
          attr(A["SNAPSHOT"], snapshot(ifx, und_addr="192.0.2.77"), nested=True))[0] == 0)
    check("commit-fails-when-the-socket-cannot-bind",
          g.cmd("COMMIT_CONFIG", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)))[0] != 0)
    check("failed-commit-published-nothing", status(g, ifx)[1]["revision"] == 0)
    check("validate", g.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)),
                            attr(A["SNAPSHOT"], snapshot(ifx), nested=True))[0] == 0)
    check("status-staged", status(g, ifx)[1]["staged"] == 1)
    check("commit-wrong-revision", g.cmd("COMMIT_CONFIG", *owner(ifx, ID1),
                                         a("CONFIG_REVISION", u64(2)))[0] == ENOENT)
    check("commit", g.cmd("COMMIT_CONFIG", *owner(ifx, ID1),
                          a("CONFIG_REVISION", u64(1)))[0] == 0)
    err, st = status(g, ifx)
    check("status-after-commit", err == 0 and st["revision"] == 1 and
          sorted(st["peers"]) == [1, 2] and st["staged"] is None)
    inc1 = st["peers"][1]["inc"]
    time.sleep(0.2)  # Let the reclamation of the retired state run.
    check("commit-stale-revision", g.cmd("VALIDATE", *owner(ifx, ID1),
          a("CONFIG_REVISION", u64(1)),
          attr(A["SNAPSHOT"], snapshot(ifx), nested=True))[0] == ESTALE)

    # A policy-only change keeps the session; a changed endpoint replaces the peer.
    g.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(2)),
          attr(A["SNAPSHOT"], snapshot(ifx, rules=(1,)), nested=True))
    check("commit-policy-only", g.cmd("COMMIT_CONFIG", *owner(ifx, ID1),
                                      a("CONFIG_REVISION", u64(2)))[0] == 0)
    st = status(g, ifx)[1]
    check("policy-only-keeps-incarnation", st["peers"][1]["inc"] == inc1)
    time.sleep(0.2)
    g.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(3)),
          attr(A["SNAPSHOT"], snapshot(ifx, rules=(1,), endpoint1="172.31.250.99"),
               nested=True))
    check("commit-disruptive", g.cmd("COMMIT_CONFIG", *owner(ifx, ID1),
                                     a("CONFIG_REVISION", u64(3)))[0] == 0)
    st = status(g, ifx)[1]
    check("endpoint-change-new-incarnation", st["peers"][1]["inc"] != inc1)
    inc1 = st["peers"][1]["inc"]

    # Key installation: keys are write-only and orientation is the controller's.
    def begin(inc, slot=1, given=None):
        """BEGIN_ATTEMPT: (errno, attempt_id, local_rx); the kernel chooses both values."""
        extra = [a("ATTEMPT_ID", given)] if given else []
        err, reply = g.cmd("BEGIN_ATTEMPT", *owner(ifx, ID1), a("PEER_SLOT", u16(slot)),
                           a("PEER_INCARNATION", inc), *extra)
        if err:
            return err, None, None
        return 0, reply[A["ATTEMPT_ID"]][0], struct.unpack("=Q", reply[A["LOCAL_RX"]][0])[0]

    def stage(inc, tx, rx, mode=0, slot=1, attempt=None, local_rx=None, begin_inc=None):
        """STAGE_GENERATION for an attempt the kernel issued (begun here unless one is given)."""
        if attempt is None:
            err, attempt, issued = begin(begin_inc or inc, slot)
            if err:
                return err
            local_rx = issued if local_rx is None else local_rx
        return g.cmd("STAGE_GENERATION", *owner(ifx, ID1), a("PEER_SLOT", u16(slot)),
                     a("PEER_INCARNATION", inc), a("ATTEMPT_ID", attempt),
                     a("HANDSHAKE_HASH", b"H" * 32), a("LOCAL_RX", u64(local_rx)),
                     a("REMOTE_RX", u64(0x2222)), a("TX_KEY", tx), a("RX_KEY", rx),
                     a("MODE", bytes([mode])))[0]
    k1, k2 = bytes(range(32)), bytes(range(100, 132))
    check("stage-equal-keys-refused", stage(inc1, k1, k1) == EINVAL)
    check("stage-bad-mode", stage(inc1, k1, k2, mode=2) == EINVAL)
    check("stage-wrong-incarnation", stage(b"\xee" * 16, k1, k2, begin_inc=inc1) == ESTALE)
    # The kernel issues attempts: nothing can be staged without one, with another's ID, or with a
    # locator the kernel did not issue; a begin for a stale incarnation or unknown slot issues none.
    check("stage-without-attempt", stage(inc1, k1, k2, attempt=b"A" * 16, local_rx=0x1111) == ENOENT)
    e_b, att1, rx1 = begin(inc1)
    check("begin-attempt", e_b == 0 and len(att1) == 16 and att1 != bytes(16) and rx1 != 0)
    check("begin-wrong-incarnation", begin(b"\xee" * 16)[0] == ESTALE)
    check("begin-unknown-slot", begin(inc1, slot=4)[0] == ENOENT)
    check("stage-foreign-attempt-id", stage(inc1, k1, k2, attempt=b"B" * 16, local_rx=rx1) == ENOENT)
    check("stage-foreign-locator", stage(inc1, k1, k2, attempt=att1, local_rx=rx1 ^ 1) == EINVAL)
    e_b, att2, rx2 = begin(inc1)
    check("begin-replaces-pending", e_b == 0 and att2 != att1 and rx2 != rx1)
    check("stage-replaced-attempt-refused", stage(inc1, k1, k2, attempt=att1, local_rx=rx1) == ENOENT)
    check("begin-given-id-needs-delivered-init", begin(inc1, given=b"I" * 16)[0] == ENOENT)
    check("stage", stage(inc1, k1, k2, attempt=att2, local_rx=rx2) == 0)
    # An identical repeat is acknowledged and changes nothing (the keys are not installed twice).
    check("stage-repeat-acknowledged", stage(inc1, k1, k2, attempt=att2, local_rx=rx2) == 0)
    st = status(g, ifx)[1]
    # A staged generation is a candidate: the peer is READY but nothing is current until activation.
    check("peer-ready-candidate-only", st["peers"][1]["state"] == STATE["READY"] and
          st["peers"][1]["gen"] == 0)
    check("disable", g.cmd("DISABLE", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                           a("PEER_INCARNATION", inc1))[0] == 0)
    st = status(g, ifx)[1]
    check("peer-disabled", st["peers"][1]["state"] == STATE["DISABLED"] and not st["peers"][1]["gen"])
    check("stage-after-disable", stage(inc1, k1, k2) == 0)
    check("revoke", g.cmd("REVOKE", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                          a("PEER_INCARNATION", inc1))[0] == 0)
    st = status(g, ifx)[1]
    check("revoke-new-incarnation", st["peers"][1]["inc"] != inc1 and
          st["peers"][1]["state"] == STATE["DOWN"])
    check("stage-old-incarnation-refused", stage(inc1, k1, k2) == ESTALE)

    def envelope(kind, locator, total, attempt=b"x" * 16):
        return (b"VPT1" + bytes([kind, 0]) + struct.pack(">H", total - 16) +
                struct.pack(">Q", locator) + attempt + b"x" * (total - 32))
    # Only the pending attempt's own INIT or RESPONSE may be sent.
    inc1 = status(g, ifx)[1]["peers"][1]["inc"]
    e_b, att3, _ = begin(inc1)
    check("exchange-tx-begin", e_b == 0)
    check("exchange-tx-unissued-attempt",
          g.cmd("EXCHANGE_TX", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                a("EXCHANGE_BYTES", envelope(1, 0, 224, attempt=b"Z" * 16)))[0] == ENOENT)
    # A whole INIT/RESPONSE is sent from the bound socket; the underlay here has no route to
    # the peer, so the send itself may fail with a network error, but never with EINVAL.
    for kind, locator, total, name in ((1, 0, 224, "init"), (2, 7, 176, "response")):
        err = g.cmd("EXCHANGE_TX", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                    a("EXCHANGE_BYTES", envelope(kind, locator, total, attempt=att3)))[0]
        check(f"exchange-tx-{name}-accepted", err in (0, 101, 113, 11), f"err={err}")
    for what, blob in (("wrong-magic", b"NOPE" + bytes(220)),
                       ("init-with-a-locator", envelope(1, 5, 224)),
                       ("response-without-a-locator", envelope(2, 0, 176)),
                       ("record-kind", envelope(3, 5, 224)),
                       ("short", b"x" * 100)):
        check(f"exchange-tx-{what}-refused",
              g.cmd("EXCHANGE_TX", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                    a("EXCHANGE_BYTES", blob))[0] == EINVAL)
    check("exchange-tx-unconfigured-slot",
          g.cmd("EXCHANGE_TX", *owner(ifx, ID1), a("PEER_SLOT", u16(4)),
                a("EXCHANGE_BYTES", envelope(1, 0, 224)))[0] == ENOENT)
    check("exchange-bytes-too-long",
          g.cmd("EXCHANGE_TX", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                a("EXCHANGE_BYTES", b"x" * 300))[0] == ERANGE)
    check("control-intent-unsupported",
          g.cmd("CONTROL_INTENT", *owner(ifx, ID1), a("PEER_SLOT", u16(1)),
                a("INTENT", b"\1"))[0] == EOPNOTSUPP)
    err, reply = g.cmd("COUNTERS", a("IFINDEX", u32(ifx)))
    counters = parse_attrs(reply.get(A["COUNTERS"], [b""])[0])
    check("counters", err == 0 and len(counters) >= 20)

    # Owner socket release: another protocol with the same numeric portid does
    # not revoke the lease; closing the Generic Netlink socket does.
    route = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE)
    try:
        route.bind((g.portid, 0))
        route.close()
        check("unrelated-protocol-close-keeps-lease", status(g2, ifx)[1]["owned"] == 1)
    except OSError:
        check("unrelated-protocol-bind", False, "could not bind NETLINK_ROUTE to the portid")
    g.close()
    deadline = time.time() + 3
    while time.time() < deadline:
        err, st = status(g2, ifx)
        if err == 0 and not st["owned"] and not st["peers"]:
            break
        time.sleep(0.05)
    check("owner-release-closes-lease-and-peers", st and not st["owned"] and not st["peers"] and
          st["revision"] == 0, str(st))
    check("owner-release-keeps-gateway", err == 0)

    # A new owner starts from nothing and gets a new epoch.
    check("reclaim", g2.cmd("CLAIM", *owner(ifx, ID2))[0] == 0)
    st = status(g2, ifx)[1]
    check("new-epoch", st["epoch"] == 2 and st["revision"] == 0)

    # Lease expiry is a clock comparison; the cleanup may lag.
    g2.cmd("VALIDATE", *owner(ifx, ID2), a("CONFIG_REVISION", u64(1)),
           attr(A["SNAPSHOT"], snapshot(ifx), nested=True))
    g2.cmd("COMMIT_CONFIG", *owner(ifx, ID2), a("CONFIG_REVISION", u64(1)))
    knob = "/sys/module/vpt1_foundation/parameters/debug_stall_expiry"
    stalled = os.path.exists(knob)
    if stalled:
        open(knob, "w").write("1")
    time.sleep(5.4)
    st = status(g2, ifx)[1]
    check("expiry-closes-without-cleanup", not st["owned"])
    # With cleanup stalled the lease is still recorded but past its deadline
    # (ESTALE); once cleanup ran there is no live owner at all (EPERM).
    check("renew-after-expiry-refused", g2.cmd("RENEW", *owner(ifx, ID2),
          a("CONFIG_REVISION", u64(1)))[0] == (ESTALE if stalled else EPERM))
    if stalled:
        check("cleanup-stalled", len(st["peers"]) == 2, str(st))
        open(knob, "w").write("0")
        deadline = time.time() + 3
        while time.time() < deadline and status(g2, ifx)[1]["peers"]:
            time.sleep(0.05)
        check("cleanup-after-stall", not status(g2, ifx)[1]["peers"])
    else:
        print("note: release build, cleanup-stall case skipped")
    check("claim-after-expiry-new-epoch", g2.cmd("CLAIM", *owner(ifx, ID2))[0] == 0 and
          status(g2, ifx)[1]["epoch"] == 3)

    # Removing the gateway keeps vpn0; a later claim recreates it.
    check("remove-gateway", g2.cmd("REMOVE_GATEWAY", *owner(ifx, ID2))[0] == 0)
    check("status-after-remove", status(g2, ifx)[0] == ENODEV)
    check("vpn0-survives", socket.if_nametoindex("vpn0") == ifx)
    check("claim-recreates", g2.cmd("CLAIM", *owner(ifx, ID1))[0] == 0)
    g2.cmd("VALIDATE", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)),
           attr(A["SNAPSHOT"], snapshot(ifx), nested=True))
    g2.cmd("COMMIT_CONFIG", *owner(ifx, ID1), a("CONFIG_REVISION", u64(1)))
    # Device removal with a live, committed gateway tears everything down.
    os.system("ip link del vpn0")
    check("status-after-device-delete", status(g2, ifx)[0] == ENODEV)

    print(f"AY2_GENL {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
