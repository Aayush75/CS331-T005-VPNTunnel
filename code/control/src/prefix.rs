//! Canonical IPv4 prefixes and interface addresses.
//!
//! TECHNICAL_DESIGN.md section 17: prefixes are canonical network address plus
//! length; default and unsupported addresses are rejected.

use std::fmt;
use std::net::Ipv4Addr;
use std::str::FromStr;

use thiserror::Error;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum PrefixError {
    #[error("`{0}` is not ADDRESS/LENGTH")]
    Syntax(String),
    #[error("`{0}` has a prefix length above 32")]
    Length(String),
    #[error("`{input}` is not canonical; write `{canonical}`")]
    NotCanonical { input: String, canonical: String },
    #[error("`{0}` is a default route, which is never an authorized prefix")]
    Default(String),
    #[error("`{0}` is in an unsupported address range")]
    Unsupported(String),
    #[error("`{0}` is a network or broadcast address, not a host address")]
    NotHost(String),
}

/// A canonical IPv4 prefix: no host bits set, length 1..=32, unicast range.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Ipv4Prefix {
    network: Ipv4Addr,
    len: u8,
}

/// Ranges no VPT1 profile-1 prefix, subnet or endpoint may touch: this
/// network, loopback, link-local, multicast and reserved/broadcast.
const UNSUPPORTED: [(Ipv4Addr, u8); 5] = [
    (Ipv4Addr::new(0, 0, 0, 0), 8),
    (Ipv4Addr::new(127, 0, 0, 0), 8),
    (Ipv4Addr::new(169, 254, 0, 0), 16),
    (Ipv4Addr::new(224, 0, 0, 0), 4),
    (Ipv4Addr::new(240, 0, 0, 0), 4),
];

/// True when any address of `network/len` lies in an unsupported range, not
/// only its first address: `192.0.0.0/2` contains multicast.
fn overlaps_unsupported(network: Ipv4Addr, len: u8) -> bool {
    let p = Ipv4Prefix { network, len };
    UNSUPPORTED
        .iter()
        .any(|&(network, len)| p.overlaps(Ipv4Prefix { network, len }))
}

/// True for addresses no VPT1 profile-1 prefix or endpoint may use.
pub fn is_unsupported(addr: Ipv4Addr) -> bool {
    addr.is_unspecified()
        || addr.is_loopback()
        || addr.is_link_local()
        || addr.is_multicast()
        || addr.is_broadcast()
        || addr.octets()[0] == 0
        || addr.octets()[0] >= 240
}

fn mask(len: u8) -> u32 {
    if len == 0 {
        0
    } else {
        u32::MAX << (32 - u32::from(len))
    }
}

fn split(s: &str) -> Result<(Ipv4Addr, u8), PrefixError> {
    let syntax = || PrefixError::Syntax(s.to_owned());
    let (addr, len) = s.split_once('/').ok_or_else(syntax)?;
    // Reject signs, whitespace and leading zeros that `u8::from_str` would allow.
    if len.is_empty() || len.len() > 2 || !len.bytes().all(|b| b.is_ascii_digit()) {
        return Err(syntax());
    }
    if len.len() == 2 && len.starts_with('0') {
        return Err(syntax());
    }
    let addr = Ipv4Addr::from_str(addr).map_err(|_| syntax())?;
    let len: u8 = len.parse().map_err(|_| syntax())?;
    if len > 32 {
        return Err(PrefixError::Length(s.to_owned()));
    }
    Ok((addr, len))
}

impl Ipv4Prefix {
    pub fn network(self) -> Ipv4Addr {
        self.network
    }

    pub fn prefix_len(self) -> u8 {
        self.len
    }

    /// Last address of the prefix.
    pub fn broadcast(self) -> Ipv4Addr {
        Ipv4Addr::from(u32::from(self.network) | !mask(self.len))
    }

    pub fn contains_addr(self, addr: Ipv4Addr) -> bool {
        u32::from(addr) & mask(self.len) == u32::from(self.network)
    }

    /// True when `other` lies entirely inside `self`.
    pub fn contains(self, other: Ipv4Prefix) -> bool {
        self.len <= other.len && self.contains_addr(other.network)
    }

    pub fn overlaps(self, other: Ipv4Prefix) -> bool {
        self.contains(other) || other.contains(self)
    }
}

impl FromStr for Ipv4Prefix {
    type Err = PrefixError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let (addr, len) = split(s)?;
        if len == 0 {
            return Err(PrefixError::Default(s.to_owned()));
        }
        let network = Ipv4Addr::from(u32::from(addr) & mask(len));
        if network != addr {
            return Err(PrefixError::NotCanonical {
                input: s.to_owned(),
                canonical: format!("{network}/{len}"),
            });
        }
        if overlaps_unsupported(network, len) {
            return Err(PrefixError::Unsupported(s.to_owned()));
        }
        Ok(Self { network, len })
    }
}

impl fmt::Display for Ipv4Prefix {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}/{}", self.network, self.len)
    }
}

/// An interface host address with its subnet, e.g. `10.77.1.1/24`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InterfaceAddr {
    addr: Ipv4Addr,
    subnet: Ipv4Prefix,
}

impl InterfaceAddr {
    pub fn addr(self) -> Ipv4Addr {
        self.addr
    }

    pub fn subnet(self) -> Ipv4Prefix {
        self.subnet
    }
}

impl FromStr for InterfaceAddr {
    type Err = PrefixError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let (addr, len) = split(s)?;
        if len == 0 {
            return Err(PrefixError::Default(s.to_owned()));
        }
        if is_unsupported(addr) {
            return Err(PrefixError::Unsupported(s.to_owned()));
        }
        let m = mask(len);
        let host = u32::from(addr) & !m;
        // /31 and /32 have no separate network/broadcast address (RFC 3021).
        if len <= 30 && (host == 0 || host == !m) {
            return Err(PrefixError::NotHost(s.to_owned()));
        }
        let subnet = Ipv4Prefix {
            network: Ipv4Addr::from(u32::from(addr) & m),
            len,
        };
        if overlaps_unsupported(subnet.network, len) {
            return Err(PrefixError::Unsupported(s.to_owned()));
        }
        Ok(Self { addr, subnet })
    }
}

impl fmt::Display for InterfaceAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}/{}", self.addr, self.subnet.len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(s: &str) -> Ipv4Prefix {
        s.parse().unwrap()
    }

    #[test]
    fn parses_canonical_prefixes() {
        assert_eq!(p("10.77.1.0/24").to_string(), "10.77.1.0/24");
        assert_eq!(p("10.77.1.10/32").prefix_len(), 32);
    }

    #[test]
    fn rejects_non_canonical_with_hint() {
        assert_eq!(
            "10.77.1.1/24".parse::<Ipv4Prefix>(),
            Err(PrefixError::NotCanonical {
                input: "10.77.1.1/24".into(),
                canonical: "10.77.1.0/24".into(),
            })
        );
    }

    #[test]
    fn rejects_default_bad_syntax_and_unsupported() {
        for bad in ["0.0.0.0/0", "10.0.0.0/0"] {
            assert!(matches!(
                bad.parse::<Ipv4Prefix>(),
                Err(PrefixError::Default(_))
            ));
        }
        for bad in [
            "10.0.0.0",
            "10.0.0.0/",
            "10.0.0.0/+8",
            "10.0.0.0/08",
            " 10.0.0.0/8",
            "10.0.0/8",
            "10.0.0.0/8/8",
        ] {
            assert!(
                matches!(bad.parse::<Ipv4Prefix>(), Err(PrefixError::Syntax(_))),
                "{bad}"
            );
        }
        assert!(matches!(
            "10.0.0.0/33".parse::<Ipv4Prefix>(),
            Err(PrefixError::Length(_))
        ));
        for bad in [
            "127.0.0.0/8",
            "224.0.0.0/4",
            "169.254.0.0/16",
            "240.0.0.0/4",
            // Network address is fine, but the range covers unsupported space.
            "192.0.0.0/2",
            "64.0.0.0/2",
            "128.0.0.0/1",
            "169.0.0.0/8",
            "255.255.255.255/32",
        ] {
            assert!(
                matches!(bad.parse::<Ipv4Prefix>(), Err(PrefixError::Unsupported(_))),
                "{bad}"
            );
        }
    }

    #[test]
    fn containment_and_overlap() {
        assert!(p("10.77.0.0/16").contains(p("10.77.1.0/24")));
        assert!(!p("10.77.1.0/24").contains(p("10.77.0.0/16")));
        assert!(p("10.77.1.0/24").overlaps(p("10.77.0.0/16")));
        assert!(!p("10.77.1.0/24").overlaps(p("10.77.2.0/24")));
        assert!(p("10.77.1.0/24").contains_addr("10.77.1.255".parse().unwrap()));
    }

    #[test]
    fn interface_addresses() {
        let a: InterfaceAddr = "10.77.1.1/24".parse().unwrap();
        assert_eq!(a.subnet(), p("10.77.1.0/24"));
        assert!(matches!(
            "10.77.1.0/24".parse::<InterfaceAddr>(),
            Err(PrefixError::NotHost(_))
        ));
        assert!(matches!(
            "10.77.1.255/24".parse::<InterfaceAddr>(),
            Err(PrefixError::NotHost(_))
        ));
        assert!("10.0.0.0/31".parse::<InterfaceAddr>().is_ok());
        // Host address supported, subnet reaching into loopback is not.
        assert!(matches!(
            "100.0.0.1/1".parse::<InterfaceAddr>(),
            Err(PrefixError::Unsupported(_))
        ));
    }
}
