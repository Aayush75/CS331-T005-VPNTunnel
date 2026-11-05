//! Generic Netlink transport against a real Linux kernel.
//!
//! These use only the built-in `nlctrl` family, so they need neither the
//! `vptunnel` module nor privileges. They run on every Linux test lane.
//!
//! Two tests send datagrams from a second socket to forge kernel traffic.
//! Sending to another socket's portid needs `CAP_NET_ADMIN`, so they are
//! `#[ignore]`d with that reason (shown as ignored, never a silent pass) and
//! run with `cargo test --test genl_linux -- --ignored` as root. Kernel
//! `ENOBUFS` delivery needs a kernel event producer (the `vptunnel` module)
//! and is covered only by the scripted tests in `genl::inbox`.
#![cfg(target_os = "linux")]

use std::path::Path;
use std::process::Command;
use std::time::{Duration, Instant};

use rustix::net::netlink::{self, SocketAddrNetlink};
use rustix::net::{AddressFamily, SendFlags, SocketFlags, SocketType, bind, sendto, socket_with};

use vpt_control::genl::message::{
    CTRL_ATTR_FAMILY_ID, CTRL_CMD_GETFAMILY, GENL_ID_CTRL, NLM_F_ACK, NLM_F_REQUEST, Request,
    parse_family,
};
use vpt_control::genl::socket::{GenlSocket, Stats, TransportError};

const ENOENT: i32 = 2;
const EINVAL: i32 = 22;

fn socket() -> GenlSocket {
    GenlSocket::open(Duration::from_secs(2)).expect("open NETLINK_GENERIC socket")
}

#[test]
fn resolves_the_controller_family() {
    let mut s = socket();
    assert_ne!(s.port(), 0, "kernel assigns a nonzero port");
    let info = s.resolve_family("nlctrl").unwrap();
    assert_eq!(info.id, GENL_ID_CTRL);
    assert_eq!(info.name, "nlctrl");
    assert!(info.version >= 1);
}

/// Parse iproute2's `genl ctrl get name NAME` header line, e.g.
/// `ID: 0x10  Version: 0x2  header size: 0  max attribs: 0`.
fn iproute2_family(name: &str) -> (u16, u32, u32, u32) {
    // iproute2 installs `genl` in sbin, which an unprivileged user's PATH
    // often omits (the AN1 reference image does).
    let genl = ["/usr/sbin/genl", "/sbin/genl", "/usr/bin/genl"]
        .into_iter()
        .find(|p| Path::new(p).is_file())
        .unwrap_or("genl");
    let out = Command::new(genl)
        .args(["ctrl", "get", "name", name])
        .output()
        .expect("iproute2 `genl` is required on Linux test lanes");
    assert!(out.status.success(), "genl ctrl get name {name} failed");
    let text = String::from_utf8(out.stdout).unwrap();
    let field = |label: &str| -> String {
        let rest =
            &text[text.find(label).unwrap_or_else(|| panic!("no {label:?}")) + label.len()..];
        rest.split_whitespace().next().unwrap().to_owned()
    };
    let hex = |s: String| u32::from_str_radix(s.trim_start_matches("0x"), 16).unwrap();
    (
        hex(field("ID:")) as u16,
        hex(field("Version:")),
        field("header size:").parse().unwrap(),
        field("max attribs:").parse().unwrap(),
    )
}

#[test]
fn family_info_matches_iproute2() {
    // Independent decoder: iproute2's own Generic Netlink client.
    let mut s = socket();
    let ours = s.resolve_family("nlctrl").unwrap();
    assert_eq!(
        (ours.id, ours.version, ours.hdrsize, ours.maxattr),
        iproute2_family("nlctrl")
    );
}

#[test]
fn absent_family_is_enoent() {
    let mut s = socket();
    let err = s.resolve_family("vpt-absent").unwrap_err();
    assert_eq!(err.errno(), Some(ENOENT), "{err}");
}

#[test]
fn getfamily_without_selector_is_einval() {
    let mut s = socket();
    let req = Request::new(
        GENL_ID_CTRL,
        CTRL_CMD_GETFAMILY,
        2,
        NLM_F_REQUEST | NLM_F_ACK,
    );
    let err = s.transact(req).unwrap_err();
    assert_eq!(err.errno(), Some(EINVAL), "{err}");
}

#[test]
fn unregistered_family_id_is_enoent() {
    let mut s = socket();
    // Highest u16 id; Generic Netlink allocates ids upward from 0x10.
    let req = Request::new(0xffff, 1, 1, NLM_F_REQUEST | NLM_F_ACK);
    let err = s.transact(req).unwrap_err();
    assert_eq!(err.errno(), Some(ENOENT), "{err}");
}

#[test]
fn lookup_by_id_agrees_with_lookup_by_name() {
    let mut s = socket();
    let by_name = s.resolve_family("nlctrl").unwrap();
    let mut req = Request::new(
        GENL_ID_CTRL,
        CTRL_CMD_GETFAMILY,
        2,
        NLM_F_REQUEST | NLM_F_ACK,
    );
    req.put_u16(CTRL_ATTR_FAMILY_ID, by_name.id).unwrap();
    let replies = s.transact(req).unwrap();
    assert_eq!(replies.len(), 1);
    assert_eq!(
        parse_family(&replies[0].payload, "nlctrl").unwrap(),
        by_name
    );
}

#[test]
fn one_socket_serves_many_requests_and_errors_do_not_poison_it() {
    let mut s = socket();
    for _ in 0..50 {
        assert!(s.resolve_family("nlctrl").is_ok());
        assert_eq!(
            s.resolve_family("vpt-absent").unwrap_err().errno(),
            Some(ENOENT)
        );
    }
}

#[test]
fn request_without_ack_is_refused_locally() {
    let mut s = socket();
    let req = Request::new(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 2, NLM_F_REQUEST);
    assert!(matches!(
        s.transact(req).unwrap_err(),
        TransportError::Unexpected(_)
    ));
    // The socket is still usable, and nothing stale leaks into the next reply.
    assert_eq!(s.resolve_family("nlctrl").unwrap().id, GENL_ID_CTRL);
}

#[test]
fn separate_sockets_get_separate_ports() {
    let a = socket();
    let b = socket();
    assert_ne!(a.port(), b.port());
}

#[test]
fn idle_recv_event_times_out_and_the_socket_stays_usable() {
    let mut s = socket();
    let start = Instant::now();
    assert!(s.recv_event(Duration::from_millis(150)).unwrap().is_none());
    let took = start.elapsed();
    assert!(
        took >= Duration::from_millis(100),
        "returned early: {took:?}"
    );
    assert!(took < Duration::from_secs(2), "overslept: {took:?}");
    assert!(!s.take_events_lost());
    assert_eq!(s.pending_events(), 0);
    // A reply is still recognised as the kernel's after an idle wait.
    assert_eq!(s.resolve_family("nlctrl").unwrap().id, GENL_ID_CTRL);
}

#[test]
fn normal_operation_leaves_the_counters_clean() {
    let mut s = socket();
    for _ in 0..20 {
        s.resolve_family("nlctrl").unwrap();
        assert!(s.resolve_family("vpt-absent").is_err());
    }
    // Real kernel replies are all recognised as the kernel's: none ignored as
    // foreign or stale, no events, nothing lost.
    assert_eq!(s.stats(), Stats::default());
    assert!(!s.take_events_lost());
}

/// A raw datagram as `NLMSG_ERROR` (code 0) for `seq`, or a family message.
fn raw(kind: u16, seq: u32, pid: u32, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&((16 + payload.len()) as u32).to_ne_bytes());
    out.extend_from_slice(&kind.to_ne_bytes());
    out.extend_from_slice(&0u16.to_ne_bytes());
    out.extend_from_slice(&seq.to_ne_bytes());
    out.extend_from_slice(&pid.to_ne_bytes());
    out.extend_from_slice(payload);
    out.resize(out.len().next_multiple_of(4), 0);
    out
}

fn ack_for(seq: u32, pid: u32) -> Vec<u8> {
    let original = raw(GENL_ID_CTRL, seq, 0, &[]);
    let mut payload = 0i32.to_ne_bytes().to_vec();
    payload.extend_from_slice(&original);
    raw(2, seq, pid, &payload) // NLMSG_ERROR
}

fn forge_to(port: u32, datagram: &[u8]) {
    let fd = socket_with(
        AddressFamily::NETLINK,
        SocketType::RAW,
        SocketFlags::CLOEXEC,
        Some(netlink::GENERIC),
    )
    .expect("sender socket");
    bind(&fd, &SocketAddrNetlink::new(0, 0)).expect("bind sender");
    sendto(
        &fd,
        datagram,
        SendFlags::empty(),
        &SocketAddrNetlink::new(port, 0),
    )
    .expect("sending to another portid needs CAP_NET_ADMIN (run as root)");
}

#[test]
#[ignore = "needs CAP_NET_ADMIN to send to another socket's portid; run as root"]
fn forged_event_from_another_socket_is_not_queued() {
    let mut s = socket();
    // Looks exactly like a kernel event: seq 0, a family message type.
    forge_to(s.port(), &raw(0x30, 0, 0, b"FORGED-INIT-BYTES"));
    assert!(s.recv_event(Duration::from_millis(300)).unwrap().is_none());
    assert_eq!(s.pending_events(), 0);
    assert_eq!(s.stats().foreign_datagrams, 1);
    assert_eq!(s.stats().events_queued, 0);
}

#[test]
#[ignore = "needs CAP_NET_ADMIN to send to another socket's portid; run as root"]
fn forged_ack_cannot_complete_a_request() {
    let mut s = socket();
    // The first request uses sequence 1. A forged ACK for it, queued ahead of
    // the kernel's real reply, must be skipped: if it counted, the request
    // would complete with no reply and resolve_family would fail.
    forge_to(s.port(), &ack_for(1, s.port()));
    let info = s.resolve_family("nlctrl").unwrap();
    assert_eq!(info.id, GENL_ID_CTRL);
    assert_eq!(s.stats().foreign_datagrams, 1);
}
