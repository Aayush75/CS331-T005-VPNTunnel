//! The `vptunnel` Generic Netlink messages the controller sends and reads: the SNAPSHOT
//! encoding, request builders, and the STATUS and event decoders.
//!
//! Everything here is portable (no sockets), so it is tested on every platform. The
//! layouts are the ones the kernel decodes (`kernel/include/vpt1_snapshot.h`,
//! `docs/AY2_RUNTIME.md`): fixed-size big-endian records inside plain attributes,
//! never a native struct.

use std::net::Ipv4Addr;

use thiserror::Error;

use crate::config::{Direction, GatewayConfig, Protocol, Role, VPT1_PORT};
use crate::exchange::SessionKeys;
use crate::genl::message::{MessageError, NLM_F_ACK, NLM_F_REQUEST, Request, attributes};
use crate::identity::PublicKey;
use crate::netlink_ids::*;
use crate::prefix::Ipv4Prefix;
use crate::wire::Mode;

/// Plain (not nested) members of the SNAPSHOT attribute.
const SNAP_LOCAL: u16 = 1;
const SNAP_PEER: u16 = 2;
const SNAP_RULE: u16 = 3;
const SNAP_LOCAL_LEN: usize = 58;
const SNAP_PEER_LEN: usize = 63;
const SNAP_RULE_LEN: usize = 21;

/// STATUS members.
const ST_OWNED: u16 = 1;
const ST_STOPPING: u16 = 2;
const ST_OWNER_EPOCH: u16 = 3;
const ST_LEASE_MS: u16 = 4;
const ST_REVISION: u16 = 5;
const ST_STAGED: u16 = 6;
const ST_PEER: u16 = 7;
const PST_SLOT: u16 = 1;
const PST_STATE: u16 = 2;
const PST_HAS_GEN: u16 = 3;
const PST_INCARNATION: u16 = 7;

#[derive(Debug, Error)]
pub enum VptunnelError {
    #[error("message: {0}")]
    Message(#[from] MessageError),
    #[error("snapshot: {0}")]
    Snapshot(&'static str),
    #[error("malformed {0}")]
    Malformed(&'static str),
}

// ---- SNAPSHOT --------------------------------------------------------------

fn put_member(out: &mut Vec<u8>, kind: u16, record: &[u8]) {
    out.extend_from_slice(&((4 + record.len()) as u16).to_ne_bytes());
    out.extend_from_slice(&kind.to_ne_bytes());
    out.extend_from_slice(record);
    out.resize(out.len().next_multiple_of(4), 0);
}

fn prefix_bytes(p: Ipv4Prefix) -> [u8; 5] {
    let o = p.network().octets();
    [o[0], o[1], o[2], o[3], p.prefix_len()]
}

/// The validated configuration as the kernel's SNAPSHOT payload. `lan_ifindex` and
/// `underlay_ifindex` are this host's interface indexes for the configured names.
pub fn encode_snapshot(
    config: &GatewayConfig,
    lan_ifindex: u32,
    underlay_ifindex: u32,
    local: &PublicKey,
) -> Result<Vec<u8>, VptunnelError> {
    let g = &config.gateway;
    let mut out = Vec::new();

    let mut rec = Vec::with_capacity(SNAP_LOCAL_LEN);
    rec.extend_from_slice(&lan_ifindex.to_be_bytes());
    rec.extend_from_slice(&underlay_ifindex.to_be_bytes());
    rec.extend_from_slice(&prefix_bytes(g.lan_address.subnet()));
    rec.extend_from_slice(&prefix_bytes(g.underlay_address.subnet()));
    rec.extend_from_slice(&g.lan_address.addr().octets());
    rec.extend_from_slice(&g.underlay_address.addr().octets());
    rec.extend_from_slice(local.as_bytes());
    debug_assert_eq!(rec.len(), SNAP_LOCAL_LEN);
    put_member(&mut out, SNAP_LOCAL, &rec);

    for peer in &config.peers {
        if peer.prefixes.len() > 4 {
            return Err(VptunnelError::Snapshot("more than four prefixes"));
        }
        let mut rec = Vec::with_capacity(SNAP_PEER_LEN);
        rec.push(peer.slot);
        rec.push(match peer.role {
            Role::Initiator => 1,
            Role::Responder => 0,
        });
        rec.extend_from_slice(&peer.path_mtu.to_be_bytes());
        rec.extend_from_slice(&peer.endpoint.octets());
        rec.extend_from_slice(&VPT1_PORT.to_be_bytes());
        rec.extend_from_slice(peer.public_key.as_bytes());
        rec.push(peer.prefixes.len() as u8);
        for i in 0..4 {
            match peer.prefixes.get(i) {
                Some(p) => rec.extend_from_slice(&prefix_bytes(*p)),
                None => rec.extend_from_slice(&[0; 5]),
            }
        }
        debug_assert_eq!(rec.len(), SNAP_PEER_LEN);
        put_member(&mut out, SNAP_PEER, &rec);
    }

    for rule in &config.rules {
        let slot = config
            .peers
            .get(rule.peer)
            .ok_or(VptunnelError::Snapshot("rule names an unknown peer"))?
            .slot;
        let (first, last) = rule.ports.map_or((0, 0), |p| (p.first, p.last));
        let mut rec = Vec::with_capacity(SNAP_RULE_LEN);
        rec.extend_from_slice(&rule.id.to_be_bytes());
        rec.push(slot);
        rec.push(match rule.direction {
            Direction::LocalToPeer => 1,
            Direction::PeerToLocal => 2,
        });
        rec.push(match rule.protocol {
            Protocol::Tcp => 6,
            Protocol::Udp => 17,
            Protocol::IcmpEcho => 1,
        });
        rec.extend_from_slice(&first.to_be_bytes());
        rec.extend_from_slice(&last.to_be_bytes());
        rec.extend_from_slice(&prefix_bytes(rule.source));
        rec.extend_from_slice(&prefix_bytes(rule.destination));
        debug_assert_eq!(rec.len(), SNAP_RULE_LEN);
        put_member(&mut out, SNAP_RULE, &rec);
    }
    Ok(out)
}

// ---- Requests ---------------------------------------------------------------

/// A `vptunnel` request that asks for an ACK, addressed to one gateway and, for
/// owner-gated commands, carrying the controller ID.
pub fn request(
    family: u16,
    cmd: u8,
    ifindex: u32,
    controller: Option<&[u8; ATTR_CONTROLLER_ID_LEN]>,
) -> Result<Request, VptunnelError> {
    let mut req = Request::new(family, cmd, VERSION, NLM_F_REQUEST | NLM_F_ACK);
    req.put_u32(ATTR_IFINDEX, ifindex)?;
    if let Some(id) = controller {
        req.put_bytes(ATTR_CONTROLLER_ID, id)?;
    }
    Ok(req)
}

/// STAGE_GENERATION for one completed attempt. The request buffer erases itself on drop.
pub fn stage_generation(
    mut req: Request,
    slot: u8,
    kernel_incarnation: &[u8; ATTR_PEER_INCARNATION_LEN],
    keys: &SessionKeys,
) -> Result<Request, VptunnelError> {
    req.put_u16(ATTR_PEER_SLOT, u16::from(slot))?;
    req.put_bytes(ATTR_PEER_INCARNATION, kernel_incarnation)?;
    req.put_bytes(ATTR_ATTEMPT_ID, &keys.attempt_id)?;
    req.put_bytes(ATTR_HANDSHAKE_HASH, &keys.handshake_hash)?;
    req.put_u64(ATTR_LOCAL_RX, keys.local_locator)?;
    req.put_u64(ATTR_REMOTE_RX, keys.remote_locator)?;
    req.put_bytes(ATTR_TX_KEY, &keys.tx[..])?;
    req.put_bytes(ATTR_RX_KEY, &keys.rx[..])?;
    req.put_u8(ATTR_MODE, keys.mode as u8)?;
    if keys.mode == Mode::Rekey {
        req.put_bytes(ATTR_PARENT_HASH, &keys.parent)?;
    }
    Ok(req)
}

/// REKEY_REQUEST (initiator only): ask the responder, through the kernel, for the continuity grant
/// of the attempt the kernel just issued. The grant comes back as a REKEY_GRANTED event.
pub fn rekey_request(
    mut req: Request,
    slot: u8,
    kernel_incarnation: &[u8; ATTR_PEER_INCARNATION_LEN],
    attempt: &[u8; ATTR_ATTEMPT_ID_LEN],
) -> Result<Request, VptunnelError> {
    req.put_u16(ATTR_PEER_SLOT, u16::from(slot))?;
    req.put_bytes(ATTR_PEER_INCARNATION, kernel_incarnation)?;
    req.put_bytes(ATTR_ATTEMPT_ID, attempt)?;
    Ok(req)
}

/// A REKEY_GRANTED or REKEY_NEEDED event: identifiers only, never keys.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RekeyEvent {
    /// The responder granted continuity for this attempt; the mode 1 INIT may go out.
    Granted {
        slot: u8,
        kernel_incarnation: [u8; ATTR_PEER_INCARNATION_LEN],
        attempt: [u8; ATTR_ATTEMPT_ID_LEN],
    },
    /// The kernel wants a re-key now (keys near their limits, the peer asked, or the peer closed).
    Needed {
        slot: u8,
        kernel_incarnation: [u8; ATTR_PEER_INCARNATION_LEN],
    },
}

pub fn parse_rekey_event(payload: &[u8]) -> Result<RekeyEvent, VptunnelError> {
    let cmd = payload.first().copied();
    if cmd != Some(CMD_REKEY_GRANTED) && cmd != Some(CMD_REKEY_NEEDED) {
        return Err(VptunnelError::Malformed("not a rekey event"));
    }
    let attrs = attributes(payload.get(4..).ok_or(VptunnelError::Malformed("event"))?)?;
    let find = |id| attrs.iter().find(|a| a.kind == id).map(|a| a.payload);
    let slot = find(ATTR_PEER_SLOT)
        .and_then(|b| <[u8; 2]>::try_from(b).ok())
        .map(u16::from_ne_bytes)
        .and_then(|v| u8::try_from(v).ok())
        .ok_or(VptunnelError::Malformed("event slot"))?;
    let kernel_incarnation = find(ATTR_PEER_INCARNATION)
        .and_then(|b| <[u8; ATTR_PEER_INCARNATION_LEN]>::try_from(b).ok())
        .ok_or(VptunnelError::Malformed("event incarnation"))?;
    if cmd == Some(CMD_REKEY_NEEDED) {
        return Ok(RekeyEvent::Needed {
            slot,
            kernel_incarnation,
        });
    }
    let attempt = find(ATTR_ATTEMPT_ID)
        .and_then(|b| <[u8; ATTR_ATTEMPT_ID_LEN]>::try_from(b).ok())
        .ok_or(VptunnelError::Malformed("event attempt"))?;
    Ok(RekeyEvent::Granted {
        slot,
        kernel_incarnation,
        attempt,
    })
}

/// BEGIN_ATTEMPT: ask the kernel for an attempt ID and a receive locator for one peer. An
/// initiator passes no ID and the kernel picks one; a responder passes the ID of the INIT it was
/// handed (the kernel refuses any other).
pub fn begin_attempt(
    mut req: Request,
    slot: u8,
    kernel_incarnation: &[u8; ATTR_PEER_INCARNATION_LEN],
    given: Option<&[u8; ATTR_ATTEMPT_ID_LEN]>,
) -> Result<Request, VptunnelError> {
    req.put_u16(ATTR_PEER_SLOT, u16::from(slot))?;
    req.put_bytes(ATTR_PEER_INCARNATION, kernel_incarnation)?;
    if let Some(id) = given {
        req.put_bytes(ATTR_ATTEMPT_ID, id)?;
    }
    Ok(req)
}

/// Decode the reply of BEGIN_ATTEMPT: the attempt ID and the receive locator the kernel issued.
pub fn parse_attempt(payload: &[u8]) -> Result<([u8; ATTR_ATTEMPT_ID_LEN], u64), VptunnelError> {
    let attrs = attributes(
        payload
            .get(4..)
            .ok_or(VptunnelError::Malformed("attempt"))?,
    )?;
    let mut id = None;
    let mut locator = None;
    for a in &attrs {
        match a.kind {
            ATTR_ATTEMPT_ID => {
                id = Some(
                    <[u8; ATTR_ATTEMPT_ID_LEN]>::try_from(a.payload)
                        .map_err(|_| VptunnelError::Malformed("attempt id"))?,
                );
            }
            ATTR_LOCAL_RX => locator = Some(u64_of(a.payload, "locator")?),
            _ => {}
        }
    }
    match (id, locator) {
        (Some(id), Some(l)) if l != 0 => Ok((id, l)),
        _ => Err(VptunnelError::Malformed("attempt: missing id or locator")),
    }
}

// ---- Replies and events -------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeerStatus {
    pub slot: u8,
    /// 0 down, 1 ready, 2 disabled, 3 invalid (`enum vpt1_peer_state`).
    pub state: u8,
    pub has_generation: bool,
    pub incarnation: [u8; ATTR_PEER_INCARNATION_LEN],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    pub owned: bool,
    pub stopping: bool,
    pub owner_epoch: u64,
    pub lease_ms: u64,
    pub revision: u64,
    pub staged_revision: Option<u64>,
    pub peers: Vec<PeerStatus>,
}

fn u64_of(b: &[u8], what: &'static str) -> Result<u64, VptunnelError> {
    <[u8; 8]>::try_from(b)
        .map(u64::from_ne_bytes)
        .map_err(|_| VptunnelError::Malformed(what))
}

/// Decode the payload of a STATUS reply (Generic Netlink header plus attributes).
pub fn parse_status(payload: &[u8]) -> Result<Status, VptunnelError> {
    let attrs = attributes(payload.get(4..).ok_or(VptunnelError::Malformed("status"))?)?;
    let nested = attrs
        .iter()
        .find(|a| a.kind == ATTR_STATUS)
        .ok_or(VptunnelError::Malformed("status: no STATUS attribute"))?;
    let mut st = Status {
        owned: false,
        stopping: false,
        owner_epoch: 0,
        lease_ms: 0,
        revision: 0,
        staged_revision: None,
        peers: Vec::new(),
    };
    for m in attributes(nested.payload)? {
        match m.kind {
            ST_OWNED => st.owned = m.payload.first() == Some(&1),
            ST_STOPPING => st.stopping = m.payload.first() == Some(&1),
            ST_OWNER_EPOCH => st.owner_epoch = u64_of(m.payload, "owner epoch")?,
            ST_LEASE_MS => st.lease_ms = u64_of(m.payload, "lease")?,
            ST_REVISION => st.revision = u64_of(m.payload, "revision")?,
            ST_STAGED => st.staged_revision = Some(u64_of(m.payload, "staged revision")?),
            ST_PEER => {
                let mut peer = PeerStatus {
                    slot: 0,
                    state: 0,
                    has_generation: false,
                    incarnation: [0; ATTR_PEER_INCARNATION_LEN],
                };
                for f in attributes(m.payload)? {
                    match f.kind {
                        PST_SLOT => {
                            peer.slot = <[u8; 2]>::try_from(f.payload)
                                .map(u16::from_ne_bytes)
                                .ok()
                                .and_then(|v| u8::try_from(v).ok())
                                .ok_or(VptunnelError::Malformed("peer slot"))?;
                        }
                        PST_STATE => peer.state = *f.payload.first().unwrap_or(&0),
                        PST_HAS_GEN => peer.has_generation = f.payload.first() == Some(&1),
                        PST_INCARNATION => {
                            peer.incarnation = f
                                .payload
                                .try_into()
                                .map_err(|_| VptunnelError::Malformed("peer incarnation"))?;
                        }
                        _ => {}
                    }
                }
                st.peers.push(peer);
            }
            _ => {}
        }
    }
    Ok(st)
}

/// An EXCHANGE_RX event: a whole INIT or RESPONSE from a configured peer, forwarded by
/// the kernel without any verification (only the controller can verify it).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExchangeRx {
    pub slot: u8,
    pub kernel_incarnation: [u8; ATTR_PEER_INCARNATION_LEN],
    pub bytes: Vec<u8>,
}

pub fn parse_exchange_rx(payload: &[u8]) -> Result<ExchangeRx, VptunnelError> {
    if payload.first() != Some(&CMD_EXCHANGE_RX) {
        return Err(VptunnelError::Malformed("not an EXCHANGE_RX event"));
    }
    let attrs = attributes(payload.get(4..).ok_or(VptunnelError::Malformed("event"))?)?;
    let find = |id| attrs.iter().find(|a| a.kind == id).map(|a| a.payload);
    let slot = find(ATTR_PEER_SLOT)
        .and_then(|b| <[u8; 2]>::try_from(b).ok())
        .map(u16::from_ne_bytes)
        .and_then(|v| u8::try_from(v).ok())
        .ok_or(VptunnelError::Malformed("event slot"))?;
    let kernel_incarnation = find(ATTR_PEER_INCARNATION)
        .and_then(|b| <[u8; ATTR_PEER_INCARNATION_LEN]>::try_from(b).ok())
        .ok_or(VptunnelError::Malformed("event incarnation"))?;
    let bytes = find(ATTR_EXCHANGE_BYTES)
        .ok_or(VptunnelError::Malformed("event bytes"))?
        .to_vec();
    Ok(ExchangeRx {
        slot,
        kernel_incarnation,
        bytes,
    })
}

/// The address a peer's outer endpoint is reached at (the fixed VPT1 port).
pub fn endpoint(addr: Ipv4Addr) -> (Ipv4Addr, u16) {
    (addr, VPT1_PORT)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::PrivateKey;

    fn example() -> GatewayConfig {
        let text =
            std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/examples/gw-a.toml"))
                .unwrap();
        GatewayConfig::parse(&text).unwrap()
    }

    /// Walk the encoded members the way the kernel's decoder does.
    fn members(buf: &[u8]) -> Vec<(u16, Vec<u8>)> {
        attributes(buf)
            .unwrap()
            .into_iter()
            .map(|a| (a.kind, a.payload.to_vec()))
            .collect()
    }

    #[test]
    fn snapshot_has_exact_record_sizes_and_big_endian_fields() {
        let cfg = example();
        let local = PrivateKey::generate().unwrap().public_key();
        let snap = encode_snapshot(&cfg, 7, 9, &local).unwrap();
        let m = members(&snap);
        let locals: Vec<_> = m.iter().filter(|(k, _)| *k == SNAP_LOCAL).collect();
        let peers: Vec<_> = m.iter().filter(|(k, _)| *k == SNAP_PEER).collect();
        let rules: Vec<_> = m.iter().filter(|(k, _)| *k == SNAP_RULE).collect();
        assert_eq!((locals.len(), peers.len(), rules.len()), (1, 1, 3));
        assert_eq!(locals[0].1.len(), SNAP_LOCAL_LEN);
        assert_eq!(&locals[0].1[0..8], &[0, 0, 0, 7, 0, 0, 0, 9]);
        // LAN 10.77.1.0/24, underlay 172.31.250.0/24, gateway addresses, then the key.
        assert_eq!(&locals[0].1[8..13], &[10, 77, 1, 0, 24]);
        assert_eq!(&locals[0].1[13..18], &[172, 31, 250, 0, 24]);
        assert_eq!(&locals[0].1[18..22], &[10, 77, 1, 1]);
        assert_eq!(&locals[0].1[22..26], &[172, 31, 250, 11]);
        assert_eq!(&locals[0].1[26..58], local.as_bytes());
        assert_eq!(peers[0].1.len(), SNAP_PEER_LEN);
        assert_eq!(peers[0].1[0], 1); // slot
        assert_eq!(peers[0].1[1], 1); // initiator
        assert_eq!(&peers[0].1[2..4], &1500u16.to_be_bytes());
        assert_eq!(&peers[0].1[4..8], &[172, 31, 250, 12]);
        assert_eq!(&peers[0].1[8..10], &51888u16.to_be_bytes());
        assert_eq!(peers[0].1[42], 1); // one prefix
        assert_eq!(&peers[0].1[43..48], &[10, 77, 2, 0, 24]);
        assert_eq!(&peers[0].1[48..63], &[0u8; 15]); // unused prefix slots are zero
        for r in &rules {
            assert_eq!(r.1.len(), SNAP_RULE_LEN);
        }
        assert_eq!(&rules[0].1[0..4], &1u32.to_be_bytes());
        assert_eq!(rules[0].1[5], 1); // local to peer
        assert_eq!(rules[0].1[6], 6); // tcp
        assert_eq!(&rules[0].1[7..11], &[0x1f, 0x90, 0x1f, 0x90]); // 8080
    }

    #[test]
    fn every_member_is_padded_to_four_bytes_and_the_total_decodes() {
        let cfg = example();
        let local = PrivateKey::generate().unwrap().public_key();
        let snap = encode_snapshot(&cfg, 2, 3, &local).unwrap();
        assert_eq!(snap.len() % 4, 0);
        assert!(snap.len() < ATTR_SNAPSHOT_MAX_LEN);
        assert!(attributes(&snap).is_ok());
    }

    #[test]
    fn requests_ask_for_an_ack_and_carry_the_owner() {
        let id = [7u8; 16];
        let req = request(0x30, CMD_CLAIM, 4, Some(&id)).unwrap();
        let bytes = req.finish(1);
        assert_eq!(
            u16::from_ne_bytes([bytes[6], bytes[7]]),
            NLM_F_REQUEST | NLM_F_ACK
        );
        assert_eq!(bytes[16], CMD_CLAIM);
        assert_eq!(bytes[17], VERSION);
        let attrs = attributes(&bytes[20..]).unwrap();
        assert_eq!(attrs[0].kind, ATTR_IFINDEX);
        assert_eq!(attrs[1].kind, ATTR_CONTROLLER_ID);
        assert_eq!(attrs[1].payload, &id);
    }

    #[test]
    fn a_nested_attribute_carries_the_nested_flag() {
        let mut req = request(0x30, CMD_VALIDATE, 4, None).unwrap();
        req.put_nested(ATTR_SNAPSHOT, &[1, 0, 4, 0]).unwrap();
        let bytes = req.finish(1);
        let attrs = attributes(&bytes[20..]).unwrap();
        assert!(!attrs[0].nested);
        assert!(attrs[1].nested && attrs[1].kind == ATTR_SNAPSHOT);
    }

    fn attr(kind: u16, payload: &[u8]) -> Vec<u8> {
        let mut v = ((4 + payload.len()) as u16).to_ne_bytes().to_vec();
        v.extend_from_slice(&kind.to_ne_bytes());
        v.extend_from_slice(payload);
        v.resize(v.len().next_multiple_of(4), 0);
        v
    }

    #[test]
    fn a_status_reply_is_decoded_without_trusting_unknown_members() {
        let mut peer = attr(PST_SLOT, &1u16.to_ne_bytes());
        peer.extend(attr(PST_STATE, &[1]));
        peer.extend(attr(PST_HAS_GEN, &[1]));
        peer.extend(attr(PST_INCARNATION, &[9; 16]));
        peer.extend(attr(99, &[1, 2, 3, 4])); // an unknown member is ignored
        let mut body = attr(ST_OWNED, &[1]);
        body.extend(attr(ST_STOPPING, &[0]));
        body.extend(attr(ST_OWNER_EPOCH, &5u64.to_ne_bytes()));
        body.extend(attr(ST_LEASE_MS, &4000u64.to_ne_bytes()));
        body.extend(attr(ST_REVISION, &1u64.to_ne_bytes()));
        body.extend(attr(ST_PEER | 0x8000, &peer));
        let mut payload = vec![CMD_STATUS, VERSION, 0, 0];
        payload.extend(attr(ATTR_STATUS | 0x8000, &body));
        let st = parse_status(&payload).unwrap();
        assert!(st.owned && !st.stopping);
        assert_eq!((st.owner_epoch, st.lease_ms, st.revision), (5, 4000, 1));
        assert_eq!(st.staged_revision, None);
        assert_eq!(st.peers.len(), 1);
        assert_eq!(st.peers[0].slot, 1);
        assert!(st.peers[0].has_generation);
        assert_eq!(st.peers[0].incarnation, [9; 16]);
    }

    #[test]
    fn truncated_status_and_events_are_errors_not_panics() {
        assert!(parse_status(&[1, 2]).is_err());
        assert!(parse_status(&[CMD_STATUS, 1, 0, 0]).is_err());
        assert!(parse_exchange_rx(&[CMD_STATUS, 1, 0, 0]).is_err());
        assert!(parse_exchange_rx(&[CMD_EXCHANGE_RX, 1, 0, 0]).is_err());
    }

    #[test]
    fn an_exchange_event_round_trips() {
        let mut payload = vec![CMD_EXCHANGE_RX, VERSION, 0, 0];
        payload.extend(attr(ATTR_PEER_SLOT, &2u16.to_ne_bytes()));
        payload.extend(attr(ATTR_PEER_INCARNATION, &[3; 16]));
        payload.extend(attr(ATTR_EXCHANGE_BYTES, &[0xAB; 224]));
        let ev = parse_exchange_rx(&payload).unwrap();
        assert_eq!(ev.slot, 2);
        assert_eq!(ev.kernel_incarnation, [3; 16]);
        assert_eq!(ev.bytes.len(), 224);
    }

    #[test]
    fn a_begin_reply_carries_the_kernels_id_and_locator() {
        let mut payload = vec![CMD_BEGIN_ATTEMPT, VERSION, 0, 0];
        payload.extend(attr(ATTR_ATTEMPT_ID, &[0x5a; 16]));
        payload.extend(attr(ATTR_LOCAL_RX, &0x0123_4567_89ab_cdefu64.to_ne_bytes()));
        assert_eq!(
            parse_attempt(&payload).unwrap(),
            ([0x5a; 16], 0x0123_4567_89ab_cdef)
        );
    }

    #[test]
    fn a_malformed_begin_reply_is_an_error() {
        let head = [CMD_BEGIN_ATTEMPT, VERSION, 0, 0];
        assert!(parse_attempt(&[1, 2]).is_err());
        assert!(parse_attempt(&head).is_err());
        // An ID without a locator, a zero locator, and a short ID are all refused.
        let mut only_id = head.to_vec();
        only_id.extend(attr(ATTR_ATTEMPT_ID, &[1; 16]));
        assert!(parse_attempt(&only_id).is_err());
        let mut zero = only_id.clone();
        zero.extend(attr(ATTR_LOCAL_RX, &0u64.to_ne_bytes()));
        assert!(parse_attempt(&zero).is_err());
        let mut short = head.to_vec();
        short.extend(attr(ATTR_ATTEMPT_ID, &[1; 8]));
        short.extend(attr(ATTR_LOCAL_RX, &1u64.to_ne_bytes()));
        assert!(parse_attempt(&short).is_err());
    }
    fn keys(mode: Mode, parent: [u8; 32]) -> SessionKeys {
        SessionKeys {
            role: crate::exchange::Role::Initiator,
            tx: zeroize::Zeroizing::new([0x11; 32]),
            rx: zeroize::Zeroizing::new([0x22; 32]),
            handshake_hash: [0x33; 32],
            attempt_id: [0x44; 16],
            local_locator: 0x0102_0304_0506_0708,
            remote_locator: 0x1112_1314_1516_1718,
            remote_incarnation: [0x55; 16],
            mode,
            parent,
        }
    }

    fn encoded_attrs(req: Request) -> Vec<(u16, Vec<u8>)> {
        let bytes = req.finish(1);
        members(&bytes[20..])
    }

    #[test]
    fn stage_generation_carries_the_parent_only_in_mode_one() {
        let req = request(0x30, CMD_STAGE_GENERATION, 4, None).unwrap();
        let a =
            encoded_attrs(stage_generation(req, 3, &[9; 16], &keys(Mode::Fresh, [0; 32])).unwrap());
        let kinds: Vec<u16> = a.iter().map(|(k, _)| *k).collect();
        assert_eq!(
            kinds,
            vec![
                ATTR_IFINDEX,
                ATTR_PEER_SLOT,
                ATTR_PEER_INCARNATION,
                ATTR_ATTEMPT_ID,
                ATTR_HANDSHAKE_HASH,
                ATTR_LOCAL_RX,
                ATTR_REMOTE_RX,
                ATTR_TX_KEY,
                ATTR_RX_KEY,
                ATTR_MODE
            ],
            "a fresh install has no parent"
        );
        let get = |k: u16| a.iter().find(|(x, _)| *x == k).unwrap().1.clone();
        assert_eq!(get(ATTR_PEER_SLOT), 3u16.to_ne_bytes());
        assert_eq!(get(ATTR_PEER_INCARNATION), vec![9; 16]);
        assert_eq!(get(ATTR_ATTEMPT_ID), vec![0x44; 16]);
        assert_eq!(get(ATTR_HANDSHAKE_HASH), vec![0x33; 32]);
        assert_eq!(get(ATTR_LOCAL_RX), 0x0102_0304_0506_0708u64.to_ne_bytes());
        assert_eq!(get(ATTR_REMOTE_RX), 0x1112_1314_1516_1718u64.to_ne_bytes());
        assert_eq!(get(ATTR_TX_KEY), vec![0x11; 32]);
        assert_eq!(get(ATTR_RX_KEY), vec![0x22; 32]);
        assert_eq!(get(ATTR_MODE), vec![0]);

        let req = request(0x30, CMD_STAGE_GENERATION, 4, None).unwrap();
        let b = encoded_attrs(
            stage_generation(req, 3, &[9; 16], &keys(Mode::Rekey, [0x66; 32])).unwrap(),
        );
        assert_eq!(b.last().unwrap(), &(ATTR_PARENT_HASH, vec![0x66; 32]));
        assert_eq!(b.iter().find(|(k, _)| *k == ATTR_MODE).unwrap().1, vec![1]);
    }

    #[test]
    fn rekey_request_encodes_slot_incarnation_and_attempt() {
        let req = request(0x30, CMD_REKEY_REQUEST, 4, None).unwrap();
        let req = rekey_request(req, 2, &[7; 16], &[8; 16]).unwrap();
        let bytes = req.finish(1);
        assert_eq!(bytes[16], CMD_REKEY_REQUEST);
        let a = members(&bytes[20..]);
        assert_eq!(
            a[1..].to_vec(),
            vec![
                (ATTR_PEER_SLOT, 2u16.to_ne_bytes().to_vec()),
                (ATTR_PEER_INCARNATION, vec![7; 16]),
                (ATTR_ATTEMPT_ID, vec![8; 16]),
            ]
        );
    }

    fn rekey_event(cmd: u8, with_attempt: bool) -> Vec<u8> {
        let mut payload = vec![cmd, VERSION, 0, 0];
        payload.extend(attr(ATTR_PEER_SLOT, &5u16.to_ne_bytes()));
        payload.extend(attr(ATTR_PEER_INCARNATION, &[3; 16]));
        if with_attempt {
            payload.extend(attr(ATTR_ATTEMPT_ID, &[4; 16]));
        }
        payload
    }

    #[test]
    fn granted_and_needed_events_are_told_apart() {
        assert_eq!(
            parse_rekey_event(&rekey_event(CMD_REKEY_GRANTED, true)).unwrap(),
            RekeyEvent::Granted {
                slot: 5,
                kernel_incarnation: [3; 16],
                attempt: [4; 16]
            }
        );
        assert_eq!(
            parse_rekey_event(&rekey_event(CMD_REKEY_NEEDED, false)).unwrap(),
            RekeyEvent::Needed {
                slot: 5,
                kernel_incarnation: [3; 16]
            }
        );
    }

    #[test]
    fn malformed_rekey_events_are_errors_not_panics() {
        // Empty, truncated header, unknown command, missing members.
        assert!(parse_rekey_event(&[]).is_err());
        assert!(parse_rekey_event(&[CMD_REKEY_GRANTED]).is_err());
        assert!(parse_rekey_event(&[CMD_REKEY_GRANTED, VERSION, 0, 0]).is_err());
        assert!(parse_rekey_event(&rekey_event(CMD_EXCHANGE_RX, true)).is_err());
        assert!(parse_rekey_event(&rekey_event(CMD_REKEY_GRANTED, false)).is_err());
        // A cut-off attribute list.
        let whole = rekey_event(CMD_REKEY_GRANTED, true);
        for cut in 0..whole.len() {
            let _ = parse_rekey_event(&whole[..cut]); // must not panic
        }
        assert!(parse_rekey_event(&whole[..whole.len() - 4]).is_err());
        // Wrong sizes: a 1 byte slot, a 15 byte incarnation, a 17 byte attempt, a slot over 255.
        let build = |slot: &[u8], inc: &[u8], att: &[u8]| {
            let mut v = vec![CMD_REKEY_GRANTED, VERSION, 0, 0];
            v.extend(attr(ATTR_PEER_SLOT, slot));
            v.extend(attr(ATTR_PEER_INCARNATION, inc));
            v.extend(attr(ATTR_ATTEMPT_ID, att));
            v
        };
        assert!(parse_rekey_event(&build(&5u16.to_ne_bytes(), &[3; 16], &[4; 16])).is_ok());
        assert!(parse_rekey_event(&build(&[5], &[3; 16], &[4; 16])).is_err());
        assert!(parse_rekey_event(&build(&5u16.to_ne_bytes(), &[3; 15], &[4; 16])).is_err());
        assert!(parse_rekey_event(&build(&5u16.to_ne_bytes(), &[3; 16], &[4; 17])).is_err());
        assert!(parse_rekey_event(&build(&256u16.to_ne_bytes(), &[3; 16], &[4; 16])).is_err());
    }
}
