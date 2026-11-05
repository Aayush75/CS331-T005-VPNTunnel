//! Bounded top-level Netlink attribute decoding for the future controller.
//!
//! Integer payloads use native byte order. This decoder does not parse Netlink
//! message headers or nested payloads and does not authorize a request, validate
//! command-specific required fields, or install keys. Borrowed payloads may
//! contain secrets, so `Attributes` deliberately does not implement `Debug`.
//!
//! This is the decoder for the `vptunnel` attribute space (ids 1..=19, each with a
//! fixed type and length). It is deliberately separate from the generic walker in
//! `genl::message::attributes`, which reads other families' attribute spaces such
//! as the `nlctrl` replies and has no schema: the two decode different id spaces,
//! so they are not duplicates of one contract.

use crate::netlink_ids::*;

const HEADER_LEN: usize = 4;
const NLA_F_NESTED: u16 = 1 << 15;
const NLA_F_NET_BYTEORDER: u16 = 1 << 14;
const NLA_TYPE_MASK: u16 = !(NLA_F_NESTED | NLA_F_NET_BYTEORDER);
const LAST_ATTRIBUTE: usize = ATTR_REQUEST_ID as usize;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AttributeError {
    TooLarge,
    TruncatedHeader,
    InvalidLength,
    UnsupportedFlags,
    UnknownAttribute,
    DuplicateAttribute,
    InvalidPayload,
}

/// Top-level attribute slices borrowed from the caller's message buffer.
///
/// A successful decode validates only top-level framing and documented scalar
/// sizes. Nested attributes and command/state semantics remain the caller's
/// responsibility; do not treat this result as an authorized message.
pub struct Attributes<'a> {
    values: [Option<&'a [u8]>; LAST_ATTRIBUTE + 1],
}

impl<'a> Attributes<'a> {
    /// Decode one aligned top-level attribute area. The caller must separately
    /// validate the complete Netlink/Generic Netlink message and its 64 KiB cap.
    pub fn decode(data: &'a [u8]) -> Result<Self, AttributeError> {
        if data.len() > MAX_MESSAGE_BYTES {
            return Err(AttributeError::TooLarge);
        }
        let mut result = Self {
            values: [None; LAST_ATTRIBUTE + 1],
        };
        let mut remaining = data;
        while !remaining.is_empty() {
            if remaining.len() < HEADER_LEN {
                return Err(AttributeError::TruncatedHeader);
            }
            let length = usize::from(u16::from_ne_bytes([remaining[0], remaining[1]]));
            let kind = u16::from_ne_bytes([remaining[2], remaining[3]]);
            if length < HEADER_LEN || length > remaining.len() {
                return Err(AttributeError::InvalidLength);
            }
            // The final attribute may omit its alignment padding, as the kernel's
            // `nla_ok` allows; any later attribute must start on the aligned offset.
            let aligned = ((length + 3) & !3).min(remaining.len());
            let id = kind & NLA_TYPE_MASK;
            if id == 0 || usize::from(id) > LAST_ATTRIBUTE {
                return Err(AttributeError::UnknownAttribute);
            }
            // Nested attributes must carry NLA_F_NESTED and no other attribute may.
            let nested = attr_is_nested(id);
            if kind & NLA_F_NET_BYTEORDER != 0 || (kind & NLA_F_NESTED != 0) != nested {
                return Err(AttributeError::UnsupportedFlags);
            }
            let index = usize::from(id);
            if result.values[index].is_some() {
                return Err(AttributeError::DuplicateAttribute);
            }
            let payload = &remaining[HEADER_LEN..length];
            if !valid_length(id, payload.len()) {
                return Err(AttributeError::InvalidPayload);
            }
            result.values[index] = Some(payload);
            remaining = &remaining[aligned..];
        }
        Ok(result)
    }

    /// Borrow the raw payload. Do not log keys or other sensitive contents.
    pub fn get(&self, id: u16) -> Option<&'a [u8]> {
        self.values.get(usize::from(id)).copied().flatten()
    }
}

fn valid_length(id: u16, length: usize) -> bool {
    match id {
        ATTR_MODE | ATTR_INTENT => length == 1,
        ATTR_PEER_SLOT => length == 2,
        ATTR_IFINDEX => length == 4,
        ATTR_CONFIG_REVISION | ATTR_LOCAL_RX | ATTR_REMOTE_RX | ATTR_REQUEST_ID => length == 8,
        ATTR_CONTROLLER_ID | ATTR_PEER_INCARNATION | ATTR_ATTEMPT_ID => length == 16,
        ATTR_HANDSHAKE_HASH | ATTR_TX_KEY | ATTR_RX_KEY | ATTR_PARENT_HASH => length == 32,
        ATTR_EXCHANGE_BYTES => length <= ATTR_EXCHANGE_BYTES_MAX_LEN,
        ATTR_SNAPSHOT => length <= ATTR_SNAPSHOT_MAX_LEN,
        ATTR_STATUS | ATTR_COUNTERS => true,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn attribute(kind: u16, payload: &[u8]) -> Vec<u8> {
        let length = u16::try_from(HEADER_LEN + payload.len()).unwrap();
        let mut out = Vec::from(length.to_ne_bytes());
        out.extend_from_slice(&kind.to_ne_bytes());
        out.extend_from_slice(payload);
        out.resize((out.len() + 3) & !3, 0);
        out
    }

    #[test]
    fn accepts_native_scalars_and_unaligned_u64_payload() {
        let mut data = attribute(ATTR_MODE, &[1]);
        let value = 0x0102_0304_0506_0708u64.to_ne_bytes();
        data.extend(attribute(ATTR_REQUEST_ID, &value));
        let parsed = Attributes::decode(&data).unwrap();
        assert_eq!(parsed.get(ATTR_MODE), Some(&[1][..]));
        assert_eq!(parsed.get(ATTR_REQUEST_ID), Some(&value[..]));
        assert!(parsed.get(ATTR_TX_KEY).is_none());
    }

    #[test]
    fn rejects_truncated_invalid_duplicate_and_unknown_attributes() {
        for data in [vec![1], vec![4, 0, 0], vec![0, 0, 1, 0]] {
            assert!(Attributes::decode(&data).is_err());
        }
        let mut valid = attribute(ATTR_IFINDEX, &1u32.to_ne_bytes());
        valid.pop();
        assert!(matches!(
            Attributes::decode(&valid),
            Err(AttributeError::InvalidLength)
        ));
        let mut duplicate = attribute(ATTR_MODE, &[0]);
        duplicate.extend(attribute(ATTR_MODE, &[1]));
        assert!(matches!(
            Attributes::decode(&duplicate),
            Err(AttributeError::DuplicateAttribute)
        ));
        assert!(matches!(
            Attributes::decode(&attribute(20, &[])),
            Err(AttributeError::UnknownAttribute)
        ));
        assert!(matches!(
            Attributes::decode(&attribute(ATTR_PEER_SLOT, &[1])),
            Err(AttributeError::InvalidPayload)
        ));
    }

    #[test]
    fn checks_every_truncation_and_snapshot_boundary() {
        let valid = attribute(ATTR_REQUEST_ID, &42u64.to_ne_bytes());
        assert!(Attributes::decode(&[]).is_ok());
        for end in 1..valid.len() {
            assert!(
                Attributes::decode(&valid[..end]).is_err(),
                "truncation at {end}"
            );
        }
        assert!(Attributes::decode(&valid).is_ok());
        assert!(
            Attributes::decode(&attribute(
                ATTR_SNAPSHOT | NLA_F_NESTED,
                &vec![0; ATTR_SNAPSHOT_MAX_LEN]
            ))
            .is_ok()
        );
        assert!(matches!(
            Attributes::decode(&attribute(
                ATTR_SNAPSHOT | NLA_F_NESTED,
                &vec![0; ATTR_SNAPSHOT_MAX_LEN + 1]
            )),
            Err(AttributeError::InvalidPayload)
        ));
        assert!(matches!(
            Attributes::decode(&attribute(0, &[])),
            Err(AttributeError::UnknownAttribute)
        ));
    }

    #[test]
    fn final_attribute_may_omit_padding_but_interior_ones_may_not_overlap() {
        // One-byte payload: nla_len 5, so 3 padding bytes follow when present.
        let mut unpadded = attribute(ATTR_MODE, &[1]);
        unpadded.truncate(5);
        let parsed = Attributes::decode(&unpadded).unwrap();
        assert_eq!(parsed.get(ATTR_MODE), Some(&[1][..]));
        // A cut inside the declared length is still invalid.
        assert!(matches!(
            Attributes::decode(&unpadded[..4]),
            Err(AttributeError::InvalidLength)
        ));
        // With a following attribute, the first one's padding is skipped and
        // the second is read from the aligned offset.
        let mut two = attribute(ATTR_MODE, &[1]);
        two.extend(attribute(ATTR_INTENT, &[2]));
        let parsed = Attributes::decode(&two).unwrap();
        assert_eq!(parsed.get(ATTR_MODE), Some(&[1][..]));
        assert_eq!(parsed.get(ATTR_INTENT), Some(&[2][..]));
    }

    #[test]
    fn nested_flag_is_required_on_nested_attributes_only() {
        for id in [ATTR_SNAPSHOT, ATTR_STATUS, ATTR_COUNTERS] {
            assert!(attr_is_nested(id));
            assert!(Attributes::decode(&attribute(id | NLA_F_NESTED, &[])).is_ok());
            assert!(matches!(
                Attributes::decode(&attribute(id, &[])),
                Err(AttributeError::UnsupportedFlags)
            ));
        }
        assert!(!attr_is_nested(ATTR_MODE));
        assert!(matches!(
            Attributes::decode(&attribute(ATTR_MODE | NLA_F_NESTED, &[0])),
            Err(AttributeError::UnsupportedFlags)
        ));
    }

    #[test]
    fn checks_flags_fixed_lengths_and_bounds() {
        assert!(Attributes::decode(&attribute(ATTR_SNAPSHOT | NLA_F_NESTED, &[])).is_ok());
        assert!(matches!(
            Attributes::decode(&attribute(ATTR_MODE | NLA_F_NESTED, &[0])),
            Err(AttributeError::UnsupportedFlags)
        ));
        assert!(matches!(
            Attributes::decode(&attribute(ATTR_IFINDEX | NLA_F_NET_BYTEORDER, &[0; 4])),
            Err(AttributeError::UnsupportedFlags)
        ));
        assert!(Attributes::decode(&attribute(ATTR_TX_KEY, &[0; 32])).is_ok());
        assert!(matches!(
            Attributes::decode(&attribute(ATTR_TX_KEY, &[0; 31])),
            Err(AttributeError::InvalidPayload)
        ));
        assert!(Attributes::decode(&attribute(ATTR_EXCHANGE_BYTES, &[0; 224])).is_ok());
        assert!(matches!(
            Attributes::decode(&attribute(ATTR_EXCHANGE_BYTES, &[0; 225])),
            Err(AttributeError::InvalidPayload)
        ));
        assert!(matches!(
            Attributes::decode(&vec![0; MAX_MESSAGE_BYTES + 1]),
            Err(AttributeError::TooLarge)
        ));
    }
}
