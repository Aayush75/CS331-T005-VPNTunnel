//! Generic Netlink message framing, independent of any one family.
//!
//! Netlink headers and attributes use the host's native byte order and 4-byte
//! alignment (TECHNICAL_DESIGN.md section 17), unlike the big-endian VPT1 wire
//! protocol. Every parse is bounded by the received buffer and the 64 KiB
//! message limit; nothing is copied from native structures.
//!
//! `vptunnel`-specific attribute validation lives in its own decoder; this
//! module only frames messages and walks attributes generically.

use thiserror::Error;
use zeroize::Zeroizing;

pub const NLMSG_HDRLEN: usize = 16;
pub const GENL_HDRLEN: usize = 4;
pub const NLA_HDRLEN: usize = 4;
/// Section 17: every Netlink message, headers included, is at most 64 KiB.
pub const MAX_MESSAGE_BYTES: usize = 64 * 1024;

pub const NLMSG_NOOP: u16 = 1;
pub const NLMSG_ERROR: u16 = 2;
pub const NLMSG_DONE: u16 = 3;
pub const NLMSG_OVERRUN: u16 = 4;
/// Message types below this are Netlink control messages, not families.
pub const NLMSG_MIN_TYPE: u16 = 0x10;

pub const NLM_F_REQUEST: u16 = 0x01;
pub const NLM_F_MULTI: u16 = 0x02;
pub const NLM_F_ACK: u16 = 0x04;

pub const NLA_F_NESTED: u16 = 1 << 15;
pub const NLA_F_NET_BYTEORDER: u16 = 1 << 14;
pub const NLA_TYPE_MASK: u16 = !(NLA_F_NESTED | NLA_F_NET_BYTEORDER);

/// The Generic Netlink controller family (`nlctrl`).
pub const GENL_ID_CTRL: u16 = NLMSG_MIN_TYPE;
pub const CTRL_CMD_NEWFAMILY: u8 = 1;
pub const CTRL_CMD_GETFAMILY: u8 = 3;
pub const CTRL_ATTR_FAMILY_ID: u16 = 1;
pub const CTRL_ATTR_FAMILY_NAME: u16 = 2;
pub const CTRL_ATTR_VERSION: u16 = 3;
pub const CTRL_ATTR_HDRSIZE: u16 = 4;
pub const CTRL_ATTR_MAXATTR: u16 = 5;
/// `GENL_NAMSIZ`: family names including the terminating NUL.
pub const GENL_NAMSIZ: usize = 16;
const CTRL_VERSION: u8 = 2;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum MessageError {
    #[error("message would exceed {MAX_MESSAGE_BYTES} bytes")]
    TooLarge,
    #[error("attribute type {0:#x} is zero or uses flag bits")]
    AttributeType(u16),
    #[error("string attribute contains NUL")]
    InteriorNul,
    #[error("truncated {0}")]
    Truncated(&'static str),
    #[error("invalid {0} length")]
    Length(&'static str),
    #[error("malformed error message")]
    ErrorMessage,
    #[error("unexpected Netlink control message type {0}")]
    ControlType(u16),
    #[error("family name must be 1..=15 printable ASCII bytes")]
    FamilyName,
    #[error("controller reply: {0}")]
    Family(&'static str),
}

const fn align4(n: usize) -> usize {
    (n + 3) & !3
}

/// A Generic Netlink request being built. The buffer is erased on drop
/// because some requests (STAGE_GENERATION) carry raw keys.
pub struct Request {
    buf: Zeroizing<Vec<u8>>,
}

impl Request {
    pub fn new(family: u16, cmd: u8, version: u8, flags: u16) -> Self {
        // Reserve the largest legal message up front. The buffer can then never
        // reallocate, so no earlier copy of key bytes is freed without being erased.
        let mut buf = Zeroizing::new(Vec::with_capacity(MAX_MESSAGE_BYTES));
        buf.extend_from_slice(&0u32.to_ne_bytes()); // nlmsg_len, set in finish
        buf.extend_from_slice(&family.to_ne_bytes());
        buf.extend_from_slice(&flags.to_ne_bytes());
        buf.extend_from_slice(&0u32.to_ne_bytes()); // nlmsg_seq, set in finish
        buf.extend_from_slice(&0u32.to_ne_bytes()); // nlmsg_pid: kernel fills
        buf.extend_from_slice(&[cmd, version, 0, 0]);
        Self { buf }
    }

    pub fn put_bytes(&mut self, kind: u16, payload: &[u8]) -> Result<&mut Self, MessageError> {
        self.put_raw(kind, 0, payload)
    }

    /// A nested attribute: `NLA_F_NESTED` is set, as the kernel's strict validation
    /// requires for SNAPSHOT, STATUS and COUNTERS. The payload is a sequence of
    /// attributes the caller has already framed.
    pub fn put_nested(&mut self, kind: u16, payload: &[u8]) -> Result<&mut Self, MessageError> {
        self.put_raw(kind, NLA_F_NESTED, payload)
    }

    fn put_raw(
        &mut self,
        kind: u16,
        flags: u16,
        payload: &[u8],
    ) -> Result<&mut Self, MessageError> {
        if kind == 0 || kind & !NLA_TYPE_MASK != 0 {
            return Err(MessageError::AttributeType(kind));
        }
        let nla_len = NLA_HDRLEN + payload.len();
        let nla_len_u16 = u16::try_from(nla_len).map_err(|_| MessageError::TooLarge)?;
        if self.buf.len() + align4(nla_len) > MAX_MESSAGE_BYTES {
            return Err(MessageError::TooLarge);
        }
        self.buf.extend_from_slice(&nla_len_u16.to_ne_bytes());
        self.buf.extend_from_slice(&(kind | flags).to_ne_bytes());
        self.buf.extend_from_slice(payload);
        let padded = align4(self.buf.len());
        self.buf.resize(padded, 0);
        Ok(self)
    }

    pub fn put_u8(&mut self, kind: u16, v: u8) -> Result<&mut Self, MessageError> {
        self.put_bytes(kind, &[v])
    }

    pub fn put_u16(&mut self, kind: u16, v: u16) -> Result<&mut Self, MessageError> {
        self.put_bytes(kind, &v.to_ne_bytes())
    }

    pub fn put_u32(&mut self, kind: u16, v: u32) -> Result<&mut Self, MessageError> {
        self.put_bytes(kind, &v.to_ne_bytes())
    }

    pub fn put_u64(&mut self, kind: u16, v: u64) -> Result<&mut Self, MessageError> {
        self.put_bytes(kind, &v.to_ne_bytes())
    }

    /// A NUL-terminated string attribute (`NLA_NUL_STRING`).
    pub fn put_str(&mut self, kind: u16, s: &str) -> Result<&mut Self, MessageError> {
        if s.as_bytes().contains(&0) {
            return Err(MessageError::InteriorNul);
        }
        let mut v = Vec::with_capacity(s.len() + 1);
        v.extend_from_slice(s.as_bytes());
        v.push(0);
        self.put_bytes(kind, &v)
    }

    /// Set the sequence number and total length; the bytes to send.
    pub fn finish(mut self, seq: u32) -> Zeroizing<Vec<u8>> {
        let len = self.buf.len() as u32; // <= MAX_MESSAGE_BYTES, checked on put
        self.buf[0..4].copy_from_slice(&len.to_ne_bytes());
        self.buf[8..12].copy_from_slice(&seq.to_ne_bytes());
        self.buf
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    pub len: u32,
    pub kind: u16,
    pub flags: u16,
    pub seq: u32,
    pub pid: u32,
}

impl Header {
    fn parse(b: &[u8]) -> Header {
        let u32_at = |i: usize| u32::from_ne_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
        Header {
            len: u32_at(0),
            kind: u16::from_ne_bytes([b[4], b[5]]),
            flags: u16::from_ne_bytes([b[6], b[7]]),
            seq: u32_at(8),
            pid: u32_at(12),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct Message<'a> {
    pub header: Header,
    pub payload: &'a [u8],
}

/// Split one received datagram into its Netlink messages.
pub fn split_messages(buf: &[u8]) -> Result<Vec<Message<'_>>, MessageError> {
    if buf.len() > MAX_MESSAGE_BYTES {
        return Err(MessageError::TooLarge);
    }
    let mut out = Vec::new();
    let mut rest = buf;
    while !rest.is_empty() {
        if rest.len() < NLMSG_HDRLEN {
            return Err(MessageError::Truncated("message header"));
        }
        let header = Header::parse(rest);
        let len = header.len as usize;
        if len < NLMSG_HDRLEN || len > rest.len() {
            return Err(MessageError::Length("message"));
        }
        out.push(Message {
            header,
            payload: &rest[NLMSG_HDRLEN..len],
        });
        // The final message may omit its alignment padding.
        rest = &rest[align4(len).min(rest.len())..];
    }
    Ok(out)
}

/// What a received message means for a request/response exchange.
#[derive(Debug, PartialEq, Eq)]
pub enum Reply<'a> {
    /// `NLMSG_ERROR` with code 0: the request with `seq` succeeded.
    Ack {
        seq: u32,
    },
    /// `NLMSG_ERROR` with a negative code: positive errno.
    Error {
        seq: u32,
        errno: i32,
    },
    Done,
    Noop,
    /// A family message (type >= 16).
    Data(&'a Message<'a>),
}

pub fn classify<'a>(msg: &'a Message<'a>) -> Result<Reply<'a>, MessageError> {
    match msg.header.kind {
        NLMSG_ERROR => {
            // i32 error code, then at least the original request header.
            let p = msg.payload;
            if p.len() < 4 + NLMSG_HDRLEN {
                return Err(MessageError::ErrorMessage);
            }
            let code = i32::from_ne_bytes([p[0], p[1], p[2], p[3]]);
            let original = Header::parse(&p[4..]);
            match code {
                0 => Ok(Reply::Ack { seq: original.seq }),
                c if c < 0 && c != i32::MIN => Ok(Reply::Error {
                    seq: original.seq,
                    errno: -c,
                }),
                _ => Err(MessageError::ErrorMessage),
            }
        }
        NLMSG_DONE => Ok(Reply::Done),
        NLMSG_NOOP => Ok(Reply::Noop),
        k if k < NLMSG_MIN_TYPE => Err(MessageError::ControlType(k)),
        _ => Ok(Reply::Data(msg)),
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct GenlPayload<'a> {
    pub cmd: u8,
    pub version: u8,
    pub attrs: &'a [u8],
}

pub fn genl_payload(payload: &[u8]) -> Result<GenlPayload<'_>, MessageError> {
    if payload.len() < GENL_HDRLEN {
        return Err(MessageError::Truncated("genl header"));
    }
    Ok(GenlPayload {
        cmd: payload[0],
        version: payload[1],
        attrs: &payload[GENL_HDRLEN..],
    })
}

#[derive(Debug, PartialEq, Eq)]
pub struct Attribute<'a> {
    /// Type with the nested/byte-order flag bits removed.
    pub kind: u16,
    pub nested: bool,
    pub net_byteorder: bool,
    pub payload: &'a [u8],
}

/// Walk one attribute area of any family (for example `nlctrl`), without a schema.
/// Duplicates are reported, not merged; callers that require uniqueness check it
/// themselves. The `vptunnel` attribute space has its own schema-checked decoder in
/// `netlink_attrs`.
pub fn attributes(data: &[u8]) -> Result<Vec<Attribute<'_>>, MessageError> {
    let mut out = Vec::new();
    let mut rest = data;
    while !rest.is_empty() {
        if rest.len() < NLA_HDRLEN {
            return Err(MessageError::Truncated("attribute header"));
        }
        let len = usize::from(u16::from_ne_bytes([rest[0], rest[1]]));
        let raw = u16::from_ne_bytes([rest[2], rest[3]]);
        if len < NLA_HDRLEN || len > rest.len() {
            return Err(MessageError::Length("attribute"));
        }
        out.push(Attribute {
            kind: raw & NLA_TYPE_MASK,
            nested: raw & NLA_F_NESTED != 0,
            net_byteorder: raw & NLA_F_NET_BYTEORDER != 0,
            payload: &rest[NLA_HDRLEN..len],
        });
        rest = &rest[align4(len).min(rest.len())..];
    }
    Ok(out)
}

fn check_family_name(name: &str) -> Result<(), MessageError> {
    let ok =
        !name.is_empty() && name.len() < GENL_NAMSIZ && name.bytes().all(|b| b.is_ascii_graphic());
    if ok {
        Ok(())
    } else {
        Err(MessageError::FamilyName)
    }
}

/// `CTRL_CMD_GETFAMILY` by name, asking for an ACK after the reply.
pub fn get_family_request(name: &str) -> Result<Request, MessageError> {
    check_family_name(name)?;
    let mut req = Request::new(
        GENL_ID_CTRL,
        CTRL_CMD_GETFAMILY,
        CTRL_VERSION,
        NLM_F_REQUEST | NLM_F_ACK,
    );
    req.put_str(CTRL_ATTR_FAMILY_NAME, name)?;
    Ok(req)
}

/// The fields of a `CTRL_CMD_NEWFAMILY` reply this controller relies on.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FamilyInfo {
    pub id: u16,
    pub name: String,
    pub version: u32,
    pub hdrsize: u32,
    pub maxattr: u32,
}

/// Decode a controller reply and require it to describe `expected`.
pub fn parse_family(payload: &[u8], expected: &str) -> Result<FamilyInfo, MessageError> {
    let g = genl_payload(payload)?;
    if g.cmd != CTRL_CMD_NEWFAMILY {
        return Err(MessageError::Family("not a NEWFAMILY reply"));
    }
    let (mut id, mut name, mut version, mut hdrsize, mut maxattr) = (None, None, None, None, None);
    let u32_of = |p: &[u8]| -> Result<u32, MessageError> {
        <[u8; 4]>::try_from(p)
            .map(u32::from_ne_bytes)
            .map_err(|_| MessageError::Length("u32 attribute"))
    };
    for a in attributes(g.attrs)? {
        let slot_taken = match a.kind {
            CTRL_ATTR_FAMILY_ID => {
                let v = <[u8; 2]>::try_from(a.payload)
                    .map(u16::from_ne_bytes)
                    .map_err(|_| MessageError::Length("family id"))?;
                id.replace(v).is_some()
            }
            CTRL_ATTR_FAMILY_NAME => {
                let s = a
                    .payload
                    .strip_suffix(&[0])
                    .ok_or(MessageError::Family("name is not NUL-terminated"))?;
                let s = std::str::from_utf8(s)
                    .map_err(|_| MessageError::Family("name is not UTF-8"))?;
                name.replace(s.to_owned()).is_some()
            }
            CTRL_ATTR_VERSION => version.replace(u32_of(a.payload)?).is_some(),
            CTRL_ATTR_HDRSIZE => hdrsize.replace(u32_of(a.payload)?).is_some(),
            CTRL_ATTR_MAXATTR => maxattr.replace(u32_of(a.payload)?).is_some(),
            // Operations, multicast groups and policies are not needed here.
            _ => false,
        };
        if slot_taken {
            return Err(MessageError::Family("duplicate attribute"));
        }
    }
    let id = id.ok_or(MessageError::Family("missing family id"))?;
    if id < NLMSG_MIN_TYPE {
        return Err(MessageError::Family("family id in the control range"));
    }
    let name = name.ok_or(MessageError::Family("missing family name"))?;
    if name != expected {
        return Err(MessageError::Family("reply names a different family"));
    }
    Ok(FamilyInfo {
        id,
        name,
        version: version.ok_or(MessageError::Family("missing version"))?,
        hdrsize: hdrsize.unwrap_or(0),
        maxattr: maxattr.unwrap_or(0),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ne16(v: u16) -> [u8; 2] {
        v.to_ne_bytes()
    }

    #[test]
    fn a_request_never_reallocates_so_key_bytes_are_never_orphaned() {
        let mut req = Request::new(0x30, 5, 1, 0);
        let start = req.buf.as_ptr();
        assert!(req.buf.capacity() >= MAX_MESSAGE_BYTES);
        // Fill the message to its limit with attributes the size of a key.
        let key = [0xAAu8; 32];
        let mut puts = 0;
        while req.put_bytes(10, &key).is_ok() {
            puts += 1;
        }
        assert!(puts > 1000, "filled {puts} attributes");
        assert_eq!(
            req.buf.as_ptr(),
            start,
            "the buffer moved: an unerased copy was freed"
        );
        assert!(req.buf.len() <= MAX_MESSAGE_BYTES);
        assert!(matches!(
            req.put_bytes(10, &key),
            Err(MessageError::TooLarge)
        ));
    }

    fn ne32(v: u32) -> [u8; 4] {
        v.to_ne_bytes()
    }

    fn attr(kind: u16, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&ne16((NLA_HDRLEN + payload.len()) as u16));
        out.extend_from_slice(&ne16(kind));
        out.extend_from_slice(payload);
        out.resize(align4(out.len()), 0);
        out
    }

    fn message(kind: u16, flags: u16, seq: u32, pid: u32, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&ne32((NLMSG_HDRLEN + payload.len()) as u32));
        out.extend_from_slice(&ne16(kind));
        out.extend_from_slice(&ne16(flags));
        out.extend_from_slice(&ne32(seq));
        out.extend_from_slice(&ne32(pid));
        out.extend_from_slice(payload);
        out
    }

    fn newfamily(attrs: &[Vec<u8>]) -> Vec<u8> {
        let mut p = vec![CTRL_CMD_NEWFAMILY, 2, 0, 0];
        for a in attrs {
            p.extend_from_slice(a);
        }
        p
    }

    #[test]
    fn get_family_request_golden() {
        let bytes = get_family_request("nlctrl").unwrap().finish(7);
        let mut expected = Vec::new();
        expected.extend_from_slice(&ne32(32)); // 16 + 4 + attr(4 + 7 -> 12)
        expected.extend_from_slice(&ne16(GENL_ID_CTRL));
        expected.extend_from_slice(&ne16(NLM_F_REQUEST | NLM_F_ACK));
        expected.extend_from_slice(&ne32(7));
        expected.extend_from_slice(&ne32(0));
        expected.extend_from_slice(&[CTRL_CMD_GETFAMILY, 2, 0, 0]);
        expected.extend_from_slice(&ne16(11));
        expected.extend_from_slice(&ne16(CTRL_ATTR_FAMILY_NAME));
        expected.extend_from_slice(b"nlctrl\0\0");
        assert_eq!(&bytes[..], &expected[..]);
    }

    #[test]
    fn request_attributes_are_aligned_and_typed() {
        let mut r = Request::new(0x20, 5, 1, NLM_F_REQUEST);
        r.put_u8(12, 1).unwrap();
        r.put_u16(4, 3).unwrap();
        r.put_u32(1, 9).unwrap();
        r.put_u64(19, u64::MAX - 1).unwrap();
        r.put_bytes(7, &[0xaa; 32]).unwrap();
        let bytes = r.finish(1);
        assert_eq!(bytes.len() % 4, 0);
        let msgs = split_messages(&bytes).unwrap();
        assert_eq!(msgs.len(), 1);
        let g = genl_payload(msgs[0].payload).unwrap();
        assert_eq!((g.cmd, g.version), (5, 1));
        let a = attributes(g.attrs).unwrap();
        let kinds: Vec<u16> = a.iter().map(|a| a.kind).collect();
        assert_eq!(kinds, [12, 4, 1, 19, 7]);
        assert_eq!(a[0].payload, &[1]);
        assert_eq!(a[3].payload, &(u64::MAX - 1).to_ne_bytes());
        assert_eq!(a[4].payload, &[0xaa; 32]);
    }

    #[test]
    fn request_rejects_bad_types_nul_and_oversize() {
        let mut r = Request::new(0x20, 1, 1, NLM_F_REQUEST);
        assert_eq!(r.put_u8(0, 1).err(), Some(MessageError::AttributeType(0)));
        assert!(r.put_u8(NLA_F_NESTED | 1, 1).is_err());
        assert!(r.put_u8(NLA_F_NET_BYTEORDER | 1, 1).is_err());
        assert_eq!(r.put_str(2, "a\0b").err(), Some(MessageError::InteriorNul));
        assert_eq!(
            r.put_bytes(1, &vec![0; 65532]).err(),
            Some(MessageError::TooLarge)
        );
        // Fill to exactly the limit, then one more byte fails.
        let room = MAX_MESSAGE_BYTES - NLMSG_HDRLEN - GENL_HDRLEN - NLA_HDRLEN;
        let mut r = Request::new(0x20, 1, 1, NLM_F_REQUEST);
        r.put_bytes(1, &vec![0; room - NLA_HDRLEN - 4]).unwrap();
        r.put_u32(2, 0).unwrap();
        assert_eq!(r.put_u8(3, 0).err(), Some(MessageError::TooLarge));
        assert_eq!(r.finish(1).len(), MAX_MESSAGE_BYTES);
    }

    #[test]
    fn family_name_bounds() {
        assert!(get_family_request("vptunnel").is_ok());
        assert!(get_family_request(&"a".repeat(15)).is_ok());
        for bad in ["", "a\0", "has space", "\u{e9}", "abcdefghijklmnop"] {
            assert_eq!(
                get_family_request(bad).err(),
                Some(MessageError::FamilyName),
                "{bad:?}"
            );
        }
    }

    #[test]
    fn splits_multiple_messages_and_unpadded_tail() {
        let mut buf = message(0x20, 0, 1, 5, &[1, 2, 3]);
        buf.resize(align4(buf.len()), 0);
        buf.extend(message(0x21, 0, 2, 5, &[9]));
        let msgs = split_messages(&buf).unwrap();
        assert_eq!(msgs.len(), 2);
        assert_eq!(msgs[0].payload, &[1, 2, 3]);
        assert_eq!(msgs[1].header.kind, 0x21);
        assert_eq!(msgs[1].payload, &[9]);
    }

    #[test]
    fn split_rejects_bad_lengths() {
        let good = message(0x20, 0, 1, 0, &[0; 8]);
        for end in 1..good.len() {
            assert!(split_messages(&good[..end]).is_err(), "truncated at {end}");
        }
        let mut short = good.clone();
        short[0..4].copy_from_slice(&ne32(15));
        assert_eq!(
            split_messages(&short).err(),
            Some(MessageError::Length("message"))
        );
        let mut long = good.clone();
        long[0..4].copy_from_slice(&ne32(25));
        assert!(split_messages(&long).is_err());
        assert_eq!(
            split_messages(&vec![0; MAX_MESSAGE_BYTES + 1]).err(),
            Some(MessageError::TooLarge)
        );
        assert_eq!(split_messages(&[]).unwrap(), vec![]);
    }

    #[test]
    fn classifies_ack_error_and_control_types() {
        let orig = message(0x20, NLM_F_REQUEST | NLM_F_ACK, 42, 0, &[]);
        let mut ack = ne32(0).to_vec();
        ack.extend_from_slice(&orig);
        // Classify the single message in `bytes`, keeping only owned results.
        fn one(bytes: &[u8]) -> Result<String, MessageError> {
            let m = split_messages(bytes).unwrap();
            assert_eq!(m.len(), 1);
            classify(&m[0]).map(|r| match r {
                Reply::Ack { seq } => format!("ack {seq}"),
                Reply::Error { seq, errno } => format!("error {seq} {errno}"),
                Reply::Done => "done".into(),
                Reply::Noop => "noop".into(),
                Reply::Data(d) => format!("data {}", d.header.kind),
            })
        }

        assert_eq!(
            one(&message(NLMSG_ERROR, 0, 42, 9, &ack)),
            Ok("ack 42".into())
        );

        let mut err = (-2i32).to_ne_bytes().to_vec();
        err.extend_from_slice(&orig);
        assert_eq!(
            one(&message(NLMSG_ERROR, 0, 42, 9, &err)),
            Ok("error 42 2".into())
        );

        for code in [1i32, i32::MIN] {
            let mut bad = code.to_ne_bytes().to_vec();
            bad.extend_from_slice(&orig);
            assert_eq!(
                one(&message(NLMSG_ERROR, 0, 42, 9, &bad)),
                Err(MessageError::ErrorMessage)
            );
        }
        assert_eq!(
            one(&message(NLMSG_ERROR, 0, 42, 9, &ack[..19])),
            Err(MessageError::ErrorMessage)
        );

        assert_eq!(
            one(&message(NLMSG_DONE, 0, 1, 0, &[0; 4])),
            Ok("done".into())
        );
        assert_eq!(one(&message(NLMSG_NOOP, 0, 1, 0, &[])), Ok("noop".into()));
        assert_eq!(
            one(&message(NLMSG_OVERRUN, 0, 1, 0, &[])),
            Err(MessageError::ControlType(NLMSG_OVERRUN))
        );
        assert_eq!(one(&message(0x30, 0, 1, 0, &[])), Ok("data 48".into()));
    }

    #[test]
    fn attribute_walk_bounds_and_flags() {
        let mut data = attr(NLA_F_NESTED | 3, &[1, 2, 3, 4]);
        data.extend(attr(5, &[7]));
        let a = attributes(&data).unwrap();
        assert_eq!(
            (a[0].kind, a[0].nested, a[0].net_byteorder),
            (3, true, false)
        );
        assert_eq!((a[1].kind, a[1].payload), (5, &[7][..]));
        for end in 1..data.len() - 3 {
            // Every cut inside an attribute (but not inside the last padding)
            // is rejected.
            if end == 8 {
                continue; // a clean boundary between the two attributes
            }
            assert!(attributes(&data[..end]).is_err(), "cut at {end}");
        }
        let mut bad = attr(1, &[0; 4]);
        bad[0..2].copy_from_slice(&ne16(3));
        assert_eq!(
            attributes(&bad).err(),
            Some(MessageError::Length("attribute"))
        );
    }

    fn family_attrs(name: &[u8]) -> Vec<Vec<u8>> {
        vec![
            attr(CTRL_ATTR_FAMILY_ID, &ne16(0x1d)),
            attr(CTRL_ATTR_FAMILY_NAME, name),
            attr(CTRL_ATTR_VERSION, &ne32(1)),
            attr(CTRL_ATTR_HDRSIZE, &ne32(0)),
            attr(CTRL_ATTR_MAXATTR, &ne32(19)),
            attr(6, &[0; 8]), // ops: ignored
        ]
    }

    #[test]
    fn parses_family_reply() {
        let p = newfamily(&family_attrs(b"vptunnel\0"));
        assert_eq!(
            parse_family(&p, "vptunnel"),
            Ok(FamilyInfo {
                id: 0x1d,
                name: "vptunnel".into(),
                version: 1,
                hdrsize: 0,
                maxattr: 19,
            })
        );
    }

    #[test]
    fn family_reply_rejections() {
        let reply = |attrs: Vec<Vec<u8>>| newfamily(&attrs);
        let err = |p: &[u8]| parse_family(p, "vptunnel").unwrap_err();

        assert_eq!(
            err(&reply(family_attrs(b"other\0"))),
            MessageError::Family("reply names a different family")
        );
        assert_eq!(
            err(&reply(family_attrs(b"vptunnel"))),
            MessageError::Family("name is not NUL-terminated")
        );
        let mut dup = family_attrs(b"vptunnel\0");
        dup.push(attr(CTRL_ATTR_FAMILY_ID, &ne16(0x1e)));
        assert_eq!(
            err(&reply(dup)),
            MessageError::Family("duplicate attribute")
        );
        let mut missing = family_attrs(b"vptunnel\0");
        missing.remove(0);
        assert_eq!(
            err(&reply(missing)),
            MessageError::Family("missing family id")
        );
        let mut low = family_attrs(b"vptunnel\0");
        low[0] = attr(CTRL_ATTR_FAMILY_ID, &ne16(4));
        assert_eq!(
            err(&reply(low)),
            MessageError::Family("family id in the control range")
        );
        let mut short_id = family_attrs(b"vptunnel\0");
        short_id[0] = attr(CTRL_ATTR_FAMILY_ID, &[0x1d]);
        assert_eq!(err(&reply(short_id)), MessageError::Length("family id"));
        let mut no_version = family_attrs(b"vptunnel\0");
        no_version.remove(2);
        assert_eq!(
            err(&reply(no_version)),
            MessageError::Family("missing version")
        );

        let mut wrong_cmd = reply(family_attrs(b"vptunnel\0"));
        wrong_cmd[0] = CTRL_CMD_GETFAMILY;
        assert_eq!(
            err(&wrong_cmd),
            MessageError::Family("not a NEWFAMILY reply")
        );
        assert_eq!(err(&[1, 2]), MessageError::Truncated("genl header"));
    }
}
