//! VPT1 handshake framing (TECHNICAL_DESIGN.md sections 4.1 and 4.2).
//!
//! All integers are big-endian and serialized field by field; nothing here
//! depends on native struct layout. Every length is exact: anything shorter,
//! longer or with a nonzero reserved field is rejected.

use thiserror::Error;

use crate::config::{PATH_MTU_MAX, PATH_MTU_MIN};

pub const MAGIC: [u8; 4] = *b"VPT1";
pub const ENVELOPE_LEN: usize = 16;
pub const ATTEMPT_ID_LEN: usize = 16;
pub const INCARNATION_LEN: usize = 16;
pub const HASH_LEN: usize = 32;
pub const HANDSHAKE_PAYLOAD_LEN: usize = 96;
/// IK message 1: e (32) + encrypted s (48) + payload + tag (16).
pub const INIT_NOISE_LEN: usize = 96 + HANDSHAKE_PAYLOAD_LEN;
/// IK message 2: e (32) + payload + tag (16).
pub const RESPONSE_NOISE_LEN: usize = 48 + HANDSHAKE_PAYLOAD_LEN;
/// Full UDP payload sizes (section 4.2): 224 and 176 bytes.
pub const INIT_LEN: usize = ENVELOPE_LEN + ATTEMPT_ID_LEN + INIT_NOISE_LEN;
pub const RESPONSE_LEN: usize = ENVELOPE_LEN + ATTEMPT_ID_LEN + RESPONSE_NOISE_LEN;
pub const PROFILE_ID: u32 = 1;
/// Outer IPv4 + UDP + VPT1 bytes added to each inner packet (section 15).
pub const DATA_OVERHEAD: u16 = 72;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum WireError {
    #[error("wrong length: expected {expected}, got {got}")]
    Length { expected: usize, got: usize },
    #[error("bad magic")]
    Magic,
    #[error("unexpected message kind {0}")]
    Kind(u8),
    #[error("nonzero flags")]
    Flags,
    #[error("body length does not match the datagram")]
    BodyLength,
    #[error("destination locator {0}")]
    Locator(&'static str),
    #[error("payload: {0}")]
    Payload(&'static str),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    Init = 1,
    Response = 2,
    Record = 3,
}

impl Kind {
    fn noise_len(self) -> usize {
        match self {
            Kind::Init => INIT_NOISE_LEN,
            Kind::Response => RESPONSE_NOISE_LEN,
            Kind::Record => 0,
        }
    }
}

/// A parsed INIT or RESPONSE datagram, borrowing the Noise message.
#[derive(Debug, PartialEq, Eq)]
pub struct HandshakeDatagram<'a> {
    pub kind: Kind,
    pub locator: u64,
    pub attempt_id: [u8; ATTEMPT_ID_LEN],
    pub noise: &'a [u8],
}

fn be_u16(b: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([b[at], b[at + 1]])
}

fn be_u32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

fn be_u64(b: &[u8], at: usize) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[at..at + 8]);
    u64::from_be_bytes(v)
}

fn array<const N: usize>(b: &[u8], at: usize) -> [u8; N] {
    let mut v = [0u8; N];
    v.copy_from_slice(&b[at..at + N]);
    v
}

/// Serialize `envelope || attempt_id || noise` for INIT or RESPONSE.
///
/// Panics if `noise` is not the exact size for `kind`: that is a local bug,
/// never peer input.
pub fn encode_handshake(
    kind: Kind,
    locator: u64,
    attempt_id: &[u8; ATTEMPT_ID_LEN],
    noise: &[u8],
) -> Vec<u8> {
    assert!(matches!(kind, Kind::Init | Kind::Response));
    assert_eq!(noise.len(), kind.noise_len());
    let body_len = ATTEMPT_ID_LEN + noise.len();
    let mut out = Vec::with_capacity(ENVELOPE_LEN + body_len);
    out.extend_from_slice(&MAGIC);
    out.push(kind as u8);
    out.push(0);
    out.extend_from_slice(&(body_len as u16).to_be_bytes());
    out.extend_from_slice(&locator.to_be_bytes());
    out.extend_from_slice(attempt_id);
    out.extend_from_slice(noise);
    out
}

/// Parse an INIT or RESPONSE datagram with every section 4.1/4.2 rule.
pub fn parse_handshake(b: &[u8]) -> Result<HandshakeDatagram<'_>, WireError> {
    if b.len() < ENVELOPE_LEN {
        return Err(WireError::Length {
            expected: ENVELOPE_LEN,
            got: b.len(),
        });
    }
    if b[0..4] != MAGIC {
        return Err(WireError::Magic);
    }
    let kind = match b[4] {
        1 => Kind::Init,
        2 => Kind::Response,
        k => return Err(WireError::Kind(k)),
    };
    if b[5] != 0 {
        return Err(WireError::Flags);
    }
    if usize::from(be_u16(b, 6)) != b.len() - ENVELOPE_LEN {
        return Err(WireError::BodyLength);
    }
    let expected = ENVELOPE_LEN + ATTEMPT_ID_LEN + kind.noise_len();
    if b.len() != expected {
        return Err(WireError::Length {
            expected,
            got: b.len(),
        });
    }
    let locator = be_u64(b, 8);
    match (kind, locator) {
        (Kind::Init, 0) | (Kind::Response, 1..) => {}
        (Kind::Init, _) => return Err(WireError::Locator("must be zero in INIT")),
        _ => return Err(WireError::Locator("must be nonzero in RESPONSE")),
    }
    Ok(HandshakeDatagram {
        kind,
        locator,
        attempt_id: array(b, ENVELOPE_LEN),
        noise: &b[ENVELOPE_LEN + ATTEMPT_ID_LEN..],
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Mode {
    Fresh = 0,
    Rekey = 1,
}

/// The 96-byte application payload carried inside both IK messages.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HandshakePayload {
    pub mode: Mode,
    pub attempt_id: [u8; ATTEMPT_ID_LEN],
    /// The sender's controller/boot incarnation.
    pub incarnation: [u8; INCARNATION_LEN],
    pub initiator_locator: u64,
    /// Zero in INIT.
    pub responder_locator: u64,
    /// Zero for fresh; the old full handshake hash for rekey.
    pub parent: [u8; HASH_LEN],
    pub path_budget: u16,
}

impl HandshakePayload {
    pub fn encode(&self) -> [u8; HANDSHAKE_PAYLOAD_LEN] {
        let mut b = [0u8; HANDSHAKE_PAYLOAD_LEN];
        b[0] = self.mode as u8;
        // 1..8 reserved, zero.
        b[8..24].copy_from_slice(&self.attempt_id);
        b[24..40].copy_from_slice(&self.incarnation);
        b[40..48].copy_from_slice(&self.initiator_locator.to_be_bytes());
        b[48..56].copy_from_slice(&self.responder_locator.to_be_bytes());
        b[56..88].copy_from_slice(&self.parent);
        b[88..92].copy_from_slice(&PROFILE_ID.to_be_bytes());
        b[92..94].copy_from_slice(&self.path_budget.to_be_bytes());
        b[94..96].copy_from_slice(&(self.path_budget - DATA_OVERHEAD).to_be_bytes());
        b
    }

    /// Decode with the rules common to INIT and RESPONSE. Role-specific
    /// correlation (echoes, locator presence) is checked by the exchange.
    pub fn decode(b: &[u8]) -> Result<Self, WireError> {
        if b.len() != HANDSHAKE_PAYLOAD_LEN {
            return Err(WireError::Length {
                expected: HANDSHAKE_PAYLOAD_LEN,
                got: b.len(),
            });
        }
        let mode = match b[0] {
            0 => Mode::Fresh,
            1 => Mode::Rekey,
            _ => return Err(WireError::Payload("unknown mode")),
        };
        if b[1..8] != [0u8; 7] {
            return Err(WireError::Payload("reserved bytes are not zero"));
        }
        if be_u32(b, 88) != PROFILE_ID {
            return Err(WireError::Payload("unsupported operating profile"));
        }
        let path_budget = be_u16(b, 92);
        if !(PATH_MTU_MIN..=PATH_MTU_MAX).contains(&path_budget) {
            return Err(WireError::Payload("path budget outside 1280..=1500"));
        }
        if be_u16(b, 94) != path_budget - DATA_OVERHEAD {
            return Err(WireError::Payload("inner cap is not path budget - 72"));
        }
        let parent: [u8; HASH_LEN] = array(b, 56);
        match (mode, parent == [0u8; HASH_LEN]) {
            (Mode::Fresh, true) | (Mode::Rekey, false) => {}
            (Mode::Fresh, false) => return Err(WireError::Payload("fresh mode with a parent")),
            (Mode::Rekey, true) => return Err(WireError::Payload("rekey mode without a parent")),
        }
        let initiator_locator = be_u64(b, 40);
        if initiator_locator == 0 {
            return Err(WireError::Payload("initiator locator is zero"));
        }
        let incarnation: [u8; INCARNATION_LEN] = array(b, 24);
        if incarnation == [0u8; INCARNATION_LEN] {
            return Err(WireError::Payload("incarnation is zero"));
        }
        Ok(Self {
            mode,
            attempt_id: array(b, 8),
            incarnation,
            initiator_locator,
            responder_locator: be_u64(b, 48),
            parent,
            path_budget,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn payload() -> HandshakePayload {
        HandshakePayload {
            mode: Mode::Fresh,
            attempt_id: [0xa1; 16],
            incarnation: [0xb2; 16],
            initiator_locator: 0x0102_0304_0506_0708,
            responder_locator: 0,
            parent: [0; 32],
            path_budget: 1500,
        }
    }

    #[test]
    fn sizes_match_design() {
        assert_eq!(INIT_LEN, 224);
        assert_eq!(RESPONSE_LEN, 176);
        assert_eq!(INIT_NOISE_LEN, 192);
        assert_eq!(RESPONSE_NOISE_LEN, 144);
    }

    #[test]
    fn payload_golden_layout() {
        let b = payload().encode();
        let mut expected = [0u8; 96];
        expected[8..24].fill(0xa1);
        expected[24..40].fill(0xb2);
        expected[40..48].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
        expected[88..92].copy_from_slice(&[0, 0, 0, 1]);
        expected[92..94].copy_from_slice(&[0x05, 0xdc]); // 1500
        expected[94..96].copy_from_slice(&[0x05, 0x94]); // 1428
        assert_eq!(b, expected);
        assert_eq!(HandshakePayload::decode(&b), Ok(payload()));
    }

    #[test]
    fn payload_rejections() {
        let good = payload().encode();
        let mutate = |f: &dyn Fn(&mut [u8; 96])| {
            let mut b = good;
            f(&mut b);
            HandshakePayload::decode(&b)
        };
        assert!(mutate(&|b| b[0] = 2).is_err());
        assert!(mutate(&|b| b[7] = 1).is_err());
        assert!(mutate(&|b| b[91] = 2).is_err());
        assert!(mutate(&|b| b[95] ^= 1).is_err());
        assert!(mutate(&|b| b[40..48].fill(0)).is_err());
        assert!(mutate(&|b| b[24..40].fill(0)).is_err());
        assert!(mutate(&|b| b[56] = 1).is_err()); // fresh with parent
        assert!(mutate(&|b| b[0] = 1).is_err()); // rekey without parent
        assert!(
            mutate(&|b| {
                b[92..94].copy_from_slice(&1279u16.to_be_bytes());
                b[94..96].copy_from_slice(&(1279u16 - 72).to_be_bytes());
            })
            .is_err()
        );
        assert!(HandshakePayload::decode(&good[..95]).is_err());
        let mut rekey = payload();
        rekey.mode = Mode::Rekey;
        rekey.parent = [7; 32];
        assert_eq!(HandshakePayload::decode(&rekey.encode()), Ok(rekey));
    }

    #[test]
    fn envelope_golden_and_round_trip() {
        let noise = [0x5a; INIT_NOISE_LEN];
        let b = encode_handshake(Kind::Init, 0, &[0xa1; 16], &noise);
        assert_eq!(b.len(), INIT_LEN);
        assert_eq!(
            &b[..16],
            &[0x56, 0x50, 0x54, 0x31, 1, 0, 0, 208, 0, 0, 0, 0, 0, 0, 0, 0]
        );
        let d = parse_handshake(&b).unwrap();
        assert_eq!(
            (d.kind, d.locator, d.attempt_id),
            (Kind::Init, 0, [0xa1; 16])
        );
        assert_eq!(d.noise, &noise[..]);

        let r = encode_handshake(Kind::Response, 9, &[1; 16], &[0; RESPONSE_NOISE_LEN]);
        assert_eq!(r.len(), RESPONSE_LEN);
        assert_eq!(&r[6..16], &[0, 160, 0, 0, 0, 0, 0, 0, 0, 9]);
        assert_eq!(parse_handshake(&r).unwrap().locator, 9);
    }

    #[test]
    fn envelope_rejections() {
        let good = encode_handshake(Kind::Init, 0, &[1; 16], &[0; INIT_NOISE_LEN]);
        let bad = |f: &dyn Fn(&mut Vec<u8>)| {
            let mut b = good.clone();
            f(&mut b);
            parse_handshake(&b).unwrap_err()
        };
        assert_eq!(bad(&|b| b[0] = b'X'), WireError::Magic);
        assert_eq!(bad(&|b| b[4] = 3), WireError::Kind(3));
        assert_eq!(bad(&|b| b[4] = 0), WireError::Kind(0));
        assert_eq!(bad(&|b| b[5] = 1), WireError::Flags);
        assert_eq!(bad(&|b| b[7] ^= 1), WireError::BodyLength);
        assert_eq!(
            bad(&|b| b[15] = 1),
            WireError::Locator("must be zero in INIT")
        );
        assert_eq!(bad(&|b| b.push(0)), WireError::BodyLength);
        // Trailing byte with a consistent body length still fails the exact size.
        assert!(matches!(
            bad(&|b| {
                b.push(0);
                b[6..8].copy_from_slice(&209u16.to_be_bytes());
            }),
            WireError::Length {
                expected: 224,
                got: 225
            }
        ));
        // INIT-sized datagram labelled RESPONSE.
        assert!(matches!(
            bad(&|b| {
                b[4] = 2;
                b[15] = 1;
            }),
            WireError::Length { .. }
        ));
        assert!(matches!(
            parse_handshake(&good[..15]),
            Err(WireError::Length { .. })
        ));

        let resp = encode_handshake(Kind::Response, 1, &[1; 16], &[0; RESPONSE_NOISE_LEN]);
        let mut zero = resp.clone();
        zero[15] = 0;
        assert_eq!(
            parse_handshake(&zero).unwrap_err(),
            WireError::Locator("must be nonzero in RESPONSE")
        );
    }
}
