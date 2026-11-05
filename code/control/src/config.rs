//! Gateway configuration: strict TOML parsing into a validated snapshot.
//!
//! Limits and semantics follow TECHNICAL_DESIGN.md sections 1, 3, 15 and 17:
//! at most four peers, four non-overlapping prefixes per peer and 128 ACL
//! rules; canonical prefixes; explicit allow-union with default denial; pair
//! path budget 1280..=1500. Unknown and duplicate fields are rejected by the
//! parser; everything else is rejected here before any snapshot exists.

use std::collections::HashSet;
use std::fs::File;
use std::io::Read;
use std::net::Ipv4Addr;
use std::path::{Path, PathBuf};

use serde::Deserialize;
use thiserror::Error;

use crate::identity::PublicKey;
use crate::prefix::{InterfaceAddr, Ipv4Prefix, is_unsupported};

pub const MAX_CONFIG_BYTES: u64 = 64 * 1024;
pub const MAX_PEERS: usize = 4;
pub const MAX_PREFIXES_PER_PEER: usize = 4;
pub const MAX_RULES: usize = 128;
pub const PATH_MTU_MIN: u16 = 1280;
pub const PATH_MTU_MAX: u16 = 1500;
pub const PATH_MTU_DEFAULT: u16 = 1500;
/// VPT1 outer UDP port, fixed for both source and destination.
pub const VPT1_PORT: u16 = 51888;
const IFNAMSIZ: usize = 16;
const MAX_NAME_LEN: usize = 32;

#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("{path}: {source}")]
    Io {
        path: String,
        #[source]
        source: std::io::Error,
    },
    #[error("configuration exceeds {MAX_CONFIG_BYTES} bytes")]
    TooLarge,
    #[error("{0}")]
    Parse(String),
    #[error("{field}: {reason}")]
    Invalid { field: String, reason: String },
}

fn invalid(field: impl Into<String>, reason: impl ToString) -> ConfigError {
    ConfigError::Invalid {
        field: field.into(),
        reason: reason.to_string(),
    }
}

// ---- Raw file schema -------------------------------------------------------

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawConfig {
    gateway: RawGateway,
    #[serde(default, rename = "peer")]
    peers: Vec<RawPeer>,
    #[serde(default, rename = "rule")]
    rules: Vec<RawRule>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawGateway {
    name: String,
    private_key_file: PathBuf,
    lan_interface: String,
    lan_address: String,
    underlay_interface: String,
    underlay_address: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawPeer {
    name: String,
    slot: u8,
    public_key: String,
    endpoint: String,
    role: Role,
    path_mtu: Option<u16>,
    prefixes: Vec<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawRule {
    id: u32,
    peer: String,
    source: String,
    destination: String,
    protocol: Protocol,
    ports: Option<String>,
}

// ---- Validated snapshot ----------------------------------------------------

/// This gateway's exchange role toward one peer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    Initiator,
    Responder,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Protocol {
    Tcp,
    Udp,
    IcmpEcho,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PortRange {
    pub first: u16,
    pub last: u16,
}

/// Which side may initiate flows matched by a rule.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    LocalToPeer,
    PeerToLocal,
}

#[derive(Debug)]
pub struct Gateway {
    pub name: String,
    pub private_key_file: PathBuf,
    pub lan_interface: String,
    pub lan_address: InterfaceAddr,
    pub underlay_interface: String,
    pub underlay_address: InterfaceAddr,
}

#[derive(Debug)]
pub struct Peer {
    pub name: String,
    /// Kernel peer slot 1..=4 (`PEER_SLOT`): stated explicitly, never derived
    /// from list order.
    pub slot: u8,
    pub public_key: PublicKey,
    pub endpoint: Ipv4Addr,
    pub role: Role,
    pub path_mtu: u16,
    pub prefixes: Vec<Ipv4Prefix>,
}

#[derive(Debug)]
pub struct Rule {
    pub id: u32,
    /// Index into [`GatewayConfig::peers`].
    pub peer: usize,
    pub source: Ipv4Prefix,
    pub destination: Ipv4Prefix,
    pub protocol: Protocol,
    pub ports: Option<PortRange>,
    pub direction: Direction,
}

#[derive(Debug)]
pub struct GatewayConfig {
    pub gateway: Gateway,
    pub peers: Vec<Peer>,
    pub rules: Vec<Rule>,
}

impl GatewayConfig {
    pub fn load(path: &Path) -> Result<Self, ConfigError> {
        let io_err = |source| ConfigError::Io {
            path: path.display().to_string(),
            source,
        };
        let mut text = String::new();
        File::open(path)
            .map_err(io_err)?
            .take(MAX_CONFIG_BYTES + 1)
            .read_to_string(&mut text)
            .map_err(io_err)?;
        if text.len() as u64 > MAX_CONFIG_BYTES {
            return Err(ConfigError::TooLarge);
        }
        Self::parse(&text)
    }

    pub fn parse(text: &str) -> Result<Self, ConfigError> {
        if text.len() as u64 > MAX_CONFIG_BYTES {
            return Err(ConfigError::TooLarge);
        }
        let raw: RawConfig = toml::from_str(text).map_err(|e| ConfigError::Parse(e.to_string()))?;
        validate(raw)
    }

    /// Reject a configuration that pins the local identity as a peer.
    pub fn check_local_identity(&self, local: &PublicKey) -> Result<(), ConfigError> {
        match self.peers.iter().find(|p| p.public_key == *local) {
            Some(p) => Err(invalid(
                format!("peer.{}.public_key", p.name),
                "is this gateway's own identity",
            )),
            None => Ok(()),
        }
    }
}

// ---- Validation ------------------------------------------------------------

fn check_name(field: &str, name: &str) -> Result<(), ConfigError> {
    let ok = !name.is_empty()
        && name.len() <= MAX_NAME_LEN
        && name
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        && !name.starts_with('-');
    if ok {
        Ok(())
    } else {
        Err(invalid(
            field,
            format!("`{name}` must be 1..={MAX_NAME_LEN} of [a-z0-9-], not starting with `-`"),
        ))
    }
}

fn check_ifname(field: &str, name: &str) -> Result<(), ConfigError> {
    let ok = !name.is_empty()
        && name.len() < IFNAMSIZ
        && name != "."
        && name != ".."
        // Physical links only: never loopback or the tunnel device.
        && name != "lo"
        && name != "vpn0"
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.'));
    if ok {
        Ok(())
    } else {
        Err(invalid(
            field,
            format!("`{name}` is not a valid physical Linux interface name"),
        ))
    }
}

fn parse_ports(field: &str, s: &str) -> Result<PortRange, ConfigError> {
    let port = |p: &str| -> Result<u16, ConfigError> {
        let digits = !p.is_empty() && p.len() <= 5 && p.bytes().all(|b| b.is_ascii_digit());
        let n = if digits && !(p.len() > 1 && p.starts_with('0')) {
            p.parse::<u16>().ok()
        } else {
            None
        };
        n.filter(|&n| n != 0).ok_or_else(|| {
            invalid(
                field,
                format!("`{s}` must be PORT or FIRST-LAST in 1..=65535"),
            )
        })
    };
    let (first, last) = match s.split_once('-') {
        Some((a, b)) => (port(a)?, port(b)?),
        None => {
            let p = port(s)?;
            (p, p)
        }
    };
    if first > last {
        return Err(invalid(field, format!("`{s}` has first port above last")));
    }
    Ok(PortRange { first, last })
}

fn validate(raw: RawConfig) -> Result<GatewayConfig, ConfigError> {
    let g = raw.gateway;
    check_name("gateway.name", &g.name)?;
    check_ifname("gateway.lan_interface", &g.lan_interface)?;
    check_ifname("gateway.underlay_interface", &g.underlay_interface)?;
    if g.lan_interface == g.underlay_interface {
        return Err(invalid(
            "gateway.underlay_interface",
            "must differ from lan_interface",
        ));
    }
    if g.private_key_file.as_os_str().is_empty() {
        return Err(invalid("gateway.private_key_file", "is empty"));
    }
    let lan: InterfaceAddr = g
        .lan_address
        .parse()
        .map_err(|e| invalid("gateway.lan_address", e))?;
    let underlay: InterfaceAddr = g
        .underlay_address
        .parse()
        .map_err(|e| invalid("gateway.underlay_address", e))?;
    if lan.subnet().overlaps(underlay.subnet()) {
        return Err(invalid(
            "gateway.underlay_address",
            "LAN and underlay subnets overlap",
        ));
    }
    if lan.subnet().prefix_len() == 32 {
        return Err(invalid(
            "gateway.lan_address",
            "a /32 LAN has no host besides the gateway",
        ));
    }

    if raw.peers.is_empty() {
        return Err(invalid("peer", "at least one peer is required"));
    }
    if raw.peers.len() > MAX_PEERS {
        return Err(invalid("peer", format!("at most {MAX_PEERS} peers")));
    }

    // Every prefix this gateway knows about, with its owner, for overlap checks.
    let mut owned: Vec<(Ipv4Prefix, String)> = vec![
        (lan.subnet(), "gateway.lan_address".into()),
        (underlay.subnet(), "gateway.underlay_address".into()),
    ];
    let mut names = HashSet::new();
    let mut slots = HashSet::new();
    let mut keys = HashSet::new();
    let mut endpoints = HashSet::new();
    let mut peers = Vec::with_capacity(raw.peers.len());

    for rp in raw.peers {
        let at = |f: &str| format!("peer.{}.{f}", rp.name);
        check_name("peer.name", &rp.name)?;
        if !names.insert(rp.name.clone()) {
            return Err(invalid(at("name"), "duplicate peer name"));
        }
        if !(1..=MAX_PEERS as u8).contains(&rp.slot) || !slots.insert(rp.slot) {
            return Err(invalid(
                at("slot"),
                format!("slots must be unique and in 1..={MAX_PEERS}"),
            ));
        }
        let public_key: PublicKey = rp
            .public_key
            .parse()
            .map_err(|e| invalid(at("public_key"), e))?;
        if !keys.insert(public_key) {
            return Err(invalid(
                at("public_key"),
                "identity already pinned by another peer",
            ));
        }
        let endpoint: Ipv4Addr = rp
            .endpoint
            .parse()
            .map_err(|_| invalid(at("endpoint"), "must be a bare IPv4 address; port is fixed"))?;
        if is_unsupported(endpoint) {
            return Err(invalid(at("endpoint"), "unsupported address"));
        }
        if endpoint == underlay.addr() {
            return Err(invalid(
                at("endpoint"),
                "is this gateway's own underlay address",
            ));
        }
        if lan.subnet().contains_addr(endpoint) {
            return Err(invalid(at("endpoint"), "inside the local LAN"));
        }
        let outer = underlay.subnet();
        if outer.prefix_len() <= 30
            && (endpoint == outer.network() || endpoint == outer.broadcast())
        {
            return Err(invalid(
                at("endpoint"),
                "is the underlay network or broadcast address",
            ));
        }
        if !endpoints.insert(endpoint) {
            return Err(invalid(
                at("endpoint"),
                "endpoint already used by another peer",
            ));
        }
        let path_mtu = rp.path_mtu.unwrap_or(PATH_MTU_DEFAULT);
        if !(PATH_MTU_MIN..=PATH_MTU_MAX).contains(&path_mtu) {
            return Err(invalid(
                at("path_mtu"),
                format!("must be in {PATH_MTU_MIN}..={PATH_MTU_MAX}"),
            ));
        }
        if rp.prefixes.is_empty() || rp.prefixes.len() > MAX_PREFIXES_PER_PEER {
            return Err(invalid(
                at("prefixes"),
                format!("must list 1..={MAX_PREFIXES_PER_PEER} prefixes"),
            ));
        }
        let mut prefixes = Vec::with_capacity(rp.prefixes.len());
        for s in &rp.prefixes {
            let p: Ipv4Prefix = s.parse().map_err(|e| invalid(at("prefixes"), e))?;
            if let Some((_, owner)) = owned.iter().find(|(q, _)| q.overlaps(p)) {
                return Err(invalid(at("prefixes"), format!("{p} overlaps {owner}")));
            }
            owned.push((p, at("prefixes")));
            prefixes.push(p);
        }
        peers.push(Peer {
            name: rp.name,
            slot: rp.slot,
            public_key,
            endpoint,
            role: rp.role,
            path_mtu,
            prefixes,
        });
    }

    // An endpoint must be underlay, never inside a tunnelled prefix.
    for p in &peers {
        if let Some(q) = peers
            .iter()
            .flat_map(|q| &q.prefixes)
            .find(|q| q.contains_addr(p.endpoint))
        {
            return Err(invalid(
                format!("peer.{}.endpoint", p.name),
                format!("inside tunnelled prefix {q}"),
            ));
        }
    }

    // The kernel refuses a snapshot with no rules (an empty policy), so the controller must too:
    // accepting it would only show up as a controller that cannot start.
    if raw.rules.is_empty() {
        return Err(invalid("rule", "at least one rule is required"));
    }
    if raw.rules.len() > MAX_RULES {
        return Err(invalid("rule", format!("at most {MAX_RULES} rules")));
    }
    let mut ids = HashSet::new();
    let mut seen = HashSet::new();
    let mut rules = Vec::with_capacity(raw.rules.len());
    for rr in raw.rules {
        let at = |f: &str| format!("rule.{}.{f}", rr.id);
        if rr.id == 0 {
            return Err(invalid("rule.id", "0 is reserved"));
        }
        if !ids.insert(rr.id) {
            return Err(invalid(at("id"), "duplicate rule id"));
        }
        let peer = peers
            .iter()
            .position(|p| p.name == rr.peer)
            .ok_or_else(|| invalid(at("peer"), format!("no peer named `{}`", rr.peer)))?;
        let source: Ipv4Prefix = rr.source.parse().map_err(|e| invalid(at("source"), e))?;
        let destination: Ipv4Prefix = rr
            .destination
            .parse()
            .map_err(|e| invalid(at("destination"), e))?;
        let in_peer = |x: Ipv4Prefix| peers[peer].prefixes.iter().any(|p| p.contains(x));
        let in_lan = |x: Ipv4Prefix| lan.subnet().contains(x);
        let direction = if in_lan(source) && in_peer(destination) {
            Direction::LocalToPeer
        } else if in_peer(source) && in_lan(destination) {
            Direction::PeerToLocal
        } else {
            return Err(invalid(
                at("destination"),
                format!(
                    "a rule must go between the local LAN and a prefix of peer `{}`",
                    rr.peer
                ),
            ));
        };
        let ports = match (rr.protocol, rr.ports.as_deref()) {
            (Protocol::IcmpEcho, None) => None,
            (Protocol::IcmpEcho, Some(_)) => {
                return Err(invalid(at("ports"), "not allowed for icmp-echo"));
            }
            (_, None) => return Err(invalid(at("ports"), "required for tcp/udp")),
            (_, Some(s)) => Some(parse_ports(&at("ports"), s)?),
        };
        if !seen.insert((peer, source, destination, rr.protocol, ports)) {
            return Err(invalid(at("id"), "duplicates another rule's match"));
        }
        rules.push(Rule {
            id: rr.id,
            peer,
            source,
            destination,
            protocol: rr.protocol,
            ports,
            direction,
        });
    }

    Ok(GatewayConfig {
        gateway: Gateway {
            name: g.name,
            private_key_file: g.private_key_file,
            lan_interface: g.lan_interface,
            lan_address: lan,
            underlay_interface: g.underlay_interface,
            underlay_address: underlay,
        },
        peers,
        rules,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY_A: &str = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    const KEY_C: &str = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";

    /// Gateway B from TECHNICAL_DESIGN.md section 2, peer A only.
    fn base() -> String {
        format!(
            r#"
[gateway]
name = "gw-b"
private_key_file = "/etc/vpntunnel/gw-b.key"
lan_interface = "lan0"
lan_address = "10.77.2.1/24"
underlay_interface = "wan0"
underlay_address = "172.31.250.12/24"

[[peer]]
name = "gw-a"
slot = 1
public_key = "{KEY_A}"
endpoint = "172.31.250.11"
role = "responder"
prefixes = ["10.77.1.0/24"]

[[rule]]
id = 1
peer = "gw-a"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8080"
"#
        )
    }

    fn err(text: &str) -> String {
        GatewayConfig::parse(text).unwrap_err().to_string()
    }

    #[test]
    fn accepts_design_gateway_b() {
        let c = GatewayConfig::parse(&base()).unwrap();
        assert_eq!(c.peers.len(), 1);
        assert_eq!(c.peers[0].path_mtu, 1500);
        assert_eq!(c.rules[0].direction, Direction::PeerToLocal);
        assert_eq!(
            c.rules[0].ports,
            Some(PortRange {
                first: 8080,
                last: 8080
            })
        );
    }

    #[test]
    fn rejects_unknown_and_duplicate_fields() {
        let unknown = base().replace(
            "role = \"responder\"",
            "role = \"responder\"\nkeepalive = 5",
        );
        assert!(err(&unknown).contains("keepalive"));
        let dup = base().replace(
            "role = \"responder\"",
            "role = \"responder\"\nrole = \"initiator\"",
        );
        assert!(err(&dup).contains("duplicate"));
        let bad_role = base().replace("\"responder\"", "\"server\"");
        assert!(GatewayConfig::parse(&bad_role).is_err());
    }

    #[test]
    fn rejects_overlapping_ownership() {
        let lan = base().replace("[\"10.77.1.0/24\"]", "[\"10.77.0.0/16\"]");
        assert!(err(&lan).contains("overlaps gateway.lan_address"));
        let within = base().replace(
            "[\"10.77.1.0/24\"]",
            "[\"10.77.1.0/24\", \"10.77.1.128/25\"]",
        );
        assert!(err(&within).contains("overlaps peer.gw-a.prefixes"));
        let default = base().replace("[\"10.77.1.0/24\"]", "[\"0.0.0.0/0\"]");
        assert!(err(&default).contains("default route"));
        let noncanon = base().replace("[\"10.77.1.0/24\"]", "[\"10.77.1.1/24\"]");
        assert!(err(&noncanon).contains("not canonical"));
    }

    #[test]
    fn rejects_duplicate_identity_and_endpoint() {
        let second = |key: &str, ep: &str, pfx: &str| {
            format!(
                "{}\n[[peer]]\nname = \"gw-c\"\nslot = 2\npublic_key = \"{key}\"\nendpoint = \"{ep}\"\nrole = \"responder\"\nprefixes = [\"{pfx}\"]\n",
                base()
            )
        };
        assert!(GatewayConfig::parse(&second(KEY_C, "172.31.250.13", "10.77.3.0/24")).is_ok());
        assert!(err(&second(KEY_A, "172.31.250.13", "10.77.3.0/24")).contains("already pinned"));
        assert!(err(&second(KEY_C, "172.31.250.11", "10.77.3.0/24")).contains("already used"));
        assert!(err(&second(KEY_C, "10.77.1.5", "10.77.3.0/24")).contains("tunnelled prefix"));
    }

    #[test]
    fn enforces_peer_and_rule_limits() {
        let mut five = base();
        for i in 0..4u8 {
            five.push_str(&format!(
                "\n[[peer]]\nname = \"p{i}\"\nslot = {}\npublic_key = \"{:02x}{}\"\nendpoint = \"172.31.250.{}\"\nrole = \"initiator\"\nprefixes = [\"10.78.{i}.0/24\"]\n",
                i + 2,
                i + 1,
                &KEY_C[2..],
                20 + i
            ));
        }
        assert!(err(&five).contains("at most 4 peers"));

        let mut many = base();
        for id in 2..=129 {
            many.push_str(&format!(
                "\n[[rule]]\nid = {id}\npeer = \"gw-a\"\nsource = \"10.77.1.10/32\"\ndestination = \"10.77.2.10/32\"\nprotocol = \"tcp\"\nports = \"{id}\"\n"
            ));
        }
        assert!(err(&many).contains("at most 128 rules"));
    }

    #[test]
    fn requires_at_least_one_rule() {
        // Everything but the rules of a valid configuration: the kernel's VALIDATE refuses this.
        let no_rules = base().split("[[rule]]").next().unwrap().to_owned();
        assert!(
            err(&no_rules).contains("at least one rule"),
            "{}",
            err(&no_rules)
        );
    }

    #[test]
    fn enforces_path_mtu_bounds() {
        for (mtu, ok) in [(1279, false), (1280, true), (1500, true), (1501, false)] {
            let text = base().replace(
                "role = \"responder\"",
                &format!("role = \"responder\"\npath_mtu = {mtu}"),
            );
            assert_eq!(GatewayConfig::parse(&text).is_ok(), ok, "{mtu}");
        }
    }

    #[test]
    fn rule_direction_and_ports() {
        let local = base()
            .replace("source = \"10.77.1.10/32\"", "source = \"10.77.2.0/24\"")
            .replace(
                "destination = \"10.77.2.10/32\"",
                "destination = \"10.77.1.10/32\"",
            );
        let c = GatewayConfig::parse(&local).unwrap();
        assert_eq!(c.rules[0].direction, Direction::LocalToPeer);

        let transit = base().replace(
            "destination = \"10.77.2.10/32\"",
            "destination = \"10.77.1.11/32\"",
        );
        assert!(err(&transit).contains("between the local LAN"));

        for bad in ["0", "65536", "90-80", "08080", "+80", "80-", ""] {
            let text = base().replace("ports = \"8080\"", &format!("ports = \"{bad}\""));
            assert!(GatewayConfig::parse(&text).is_err(), "{bad}");
        }
        let range = base().replace("ports = \"8080\"", "ports = \"8000-8080\"");
        assert!(GatewayConfig::parse(&range).is_ok());

        let echo_ports = base().replace("protocol = \"tcp\"", "protocol = \"icmp-echo\"");
        assert!(err(&echo_ports).contains("not allowed for icmp-echo"));
        let echo = echo_ports.replace("ports = \"8080\"\n", "");
        assert!(GatewayConfig::parse(&echo).is_ok());
        let no_ports = base().replace("ports = \"8080\"\n", "");
        assert!(err(&no_ports).contains("required for tcp/udp"));
    }

    #[test]
    fn rejects_duplicate_rules() {
        let dup_id = format!(
            "{}\n[[rule]]\nid = 1\npeer = \"gw-a\"\nsource = \"10.77.1.11/32\"\ndestination = \"10.77.2.10/32\"\nprotocol = \"tcp\"\nports = \"8080\"\n",
            base()
        );
        assert!(err(&dup_id).contains("duplicate rule id"));
        let dup_match = dup_id.replace(
            "id = 1\npeer = \"gw-a\"\nsource = \"10.77.1.11/32\"",
            "id = 2\npeer = \"gw-a\"\nsource = \"10.77.1.10/32\"",
        );
        assert!(err(&dup_match).contains("duplicates another rule"));
    }

    #[test]
    fn rejects_bad_gateway_fields() {
        let same_if = base().replace("\"wan0\"", "\"lan0\"");
        assert!(err(&same_if).contains("must differ"));
        let long_if = base().replace("\"wan0\"", "\"abcdefghijklmnop\"");
        assert!(err(&long_if).contains("interface name"));
        let overlap = base().replace("172.31.250.12/24", "10.77.2.2/16");
        assert!(err(&overlap).contains("overlap"));
        let netaddr = base().replace("10.77.2.1/24", "10.77.2.0/24");
        assert!(err(&netaddr).contains("network or broadcast"));
        let name = base().replace("\"gw-b\"", "\"GW B\"");
        assert!(err(&name).contains("gateway.name"));
    }

    #[test]
    fn rejects_oversize_input() {
        let big = format!("{}#{}", base(), "x".repeat(MAX_CONFIG_BYTES as usize));
        assert!(matches!(
            GatewayConfig::parse(&big),
            Err(ConfigError::TooLarge)
        ));
    }

    #[test]
    fn rejects_own_identity_as_peer() {
        let c = GatewayConfig::parse(&base()).unwrap();
        assert!(c.check_local_identity(&KEY_A.parse().unwrap()).is_err());
        assert!(c.check_local_identity(&KEY_C.parse().unwrap()).is_ok());
    }

    #[test]
    fn requires_explicit_unique_slots() {
        let missing = base().replace("slot = 1\n", "");
        assert!(err(&missing).contains("slot"));
        for bad in ["0", "5", "-1", "256"] {
            let text = base().replace("slot = 1", &format!("slot = {bad}"));
            assert!(GatewayConfig::parse(&text).is_err(), "slot {bad}");
        }
        let c = GatewayConfig::parse(&base().replace("slot = 1", "slot = 4")).unwrap();
        assert_eq!(c.peers[0].slot, 4);
        let dup = format!(
            "{}\n[[peer]]\nname = \"gw-c\"\nslot = 1\npublic_key = \"{KEY_C}\"\nendpoint = \"172.31.250.13\"\nrole = \"responder\"\nprefixes = [\"10.77.3.0/24\"]\n",
            base()
        );
        assert!(err(&dup).contains("slots must be unique"));
    }

    #[test]
    fn rejects_non_physical_interfaces_and_host_only_lan() {
        for name in ["lo", "vpn0"] {
            let lan = base().replace(
                "lan_interface = \"lan0\"",
                &format!("lan_interface = \"{name}\""),
            );
            assert!(err(&lan).contains("physical"), "{name}");
            let wan = base().replace("\"wan0\"", &format!("\"{name}\""));
            assert!(err(&wan).contains("physical"), "{name}");
        }
        let host_lan = base().replace("10.77.2.1/24", "10.77.2.1/32");
        assert!(err(&host_lan).contains("/32 LAN"));
        let pair_lan = base().replace("10.77.2.1/24", "10.77.2.0/31").replace(
            "destination = \"10.77.2.10/32\"",
            "destination = \"10.77.2.1/32\"",
        );
        assert!(GatewayConfig::parse(&pair_lan).is_ok());
    }

    #[test]
    fn rejects_underlay_network_and_broadcast_endpoints() {
        for ep in ["172.31.250.0", "172.31.250.255"] {
            let text = base().replace(
                "endpoint = \"172.31.250.11\"",
                &format!("endpoint = \"{ep}\""),
            );
            assert!(err(&text).contains("network or broadcast"), "{ep}");
        }
        // A routed endpoint outside the underlay subnet is still allowed.
        let routed = base().replace(
            "endpoint = \"172.31.250.11\"",
            "endpoint = \"172.31.251.11\"",
        );
        assert!(GatewayConfig::parse(&routed).is_ok());
    }

    #[test]
    fn rejects_prefixes_that_reach_unsupported_space() {
        for pfx in ["192.0.0.0/2", "64.0.0.0/2", "128.0.0.0/1"] {
            let text = base().replace("[\"10.77.1.0/24\"]", &format!("[\"{pfx}\"]"));
            assert!(err(&text).contains("unsupported"), "{pfx}");
        }
    }
}
