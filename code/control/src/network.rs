//! The network summary of a validated gateway configuration: the LAN and underlay
//! links, and each peer's slot, endpoint and owned prefixes.
//!
//! `infra/gateway/vpt-gateway.sh` and `vpt-ap.sh` read it (`vptctl network`) so the
//! interface names, the LAN address and the peers' prefixes come from the one
//! configuration file. It carries no identity, key, rule, lease or session state and
//! installs nothing.

use serde::Serialize;

use crate::config::GatewayConfig;

#[derive(Debug, PartialEq, Eq, Serialize)]
pub struct Network {
    pub lan: Link,
    pub underlay: Link,
    pub peers: Vec<NetworkPeer>,
}

#[derive(Debug, PartialEq, Eq, Serialize)]
pub struct Link {
    pub interface: String,
    /// Gateway address with prefix length, e.g. `10.77.1.1/24`.
    pub address: String,
}

#[derive(Debug, PartialEq, Eq, Serialize)]
pub struct NetworkPeer {
    pub slot: u8,
    pub endpoint: String,
    pub prefixes: Vec<String>,
}

impl Network {
    /// Summarize a configuration that has already passed full validation.
    /// Peers are ordered by their explicit slot, not by file order.
    pub fn from_config(c: &GatewayConfig) -> Self {
        let g = &c.gateway;
        let mut peers: Vec<NetworkPeer> = c
            .peers
            .iter()
            .map(|p| NetworkPeer {
                slot: p.slot,
                endpoint: p.endpoint.to_string(),
                prefixes: p.prefixes.iter().map(ToString::to_string).collect(),
            })
            .collect();
        peers.sort_by_key(|p| p.slot);
        Network {
            lan: Link {
                interface: g.lan_interface.clone(),
                address: g.lan_address.to_string(),
            },
            underlay: Link {
                interface: g.underlay_interface.clone(),
                address: g.underlay_address.to_string(),
            },
            peers,
        }
    }

    /// Stable, human-readable JSON with a trailing newline.
    pub fn to_json(&self) -> String {
        // Serializing plain strings and integers cannot fail.
        let mut s = serde_json::to_string_pretty(self).expect("network summary serializes");
        s.push('\n');
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY_A: &str = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    const KEY_C: &str = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";

    /// Gateway B with C listed before A but holding the higher slot.
    fn config() -> GatewayConfig {
        GatewayConfig::parse(&format!(
            r#"
[gateway]
name = "gw-b"
private_key_file = "/etc/vpntunnel/gw-b.key"
lan_interface = "lan0"
lan_address = "10.77.2.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.12/24"

[[peer]]
name = "gw-c"
slot = 2
public_key = "{KEY_C}"
endpoint = "172.31.250.13"
role = "responder"
prefixes = ["10.77.3.0/24", "10.79.0.0/16"]

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
protocol = "icmp-echo"
"#
        ))
        .unwrap()
    }

    #[test]
    fn orders_by_slot_and_keeps_only_network_fields() {
        let p = Network::from_config(&config());
        assert_eq!(p.lan.address, "10.77.2.1/24");
        assert_eq!(p.underlay.interface, "out0");
        assert_eq!(
            p.peers,
            vec![
                NetworkPeer {
                    slot: 1,
                    endpoint: "172.31.250.11".into(),
                    prefixes: vec!["10.77.1.0/24".into()],
                },
                NetworkPeer {
                    slot: 2,
                    endpoint: "172.31.250.13".into(),
                    prefixes: vec!["10.77.3.0/24".into(), "10.79.0.0/16".into()],
                },
            ]
        );
    }

    #[test]
    fn json_has_exactly_the_schema_fields_and_no_secrets() {
        let json = Network::from_config(&config()).to_json();
        let v: serde_json::Value = serde_json::from_str(&json).unwrap();
        let keys = |o: &serde_json::Value| {
            let mut k: Vec<String> = o.as_object().unwrap().keys().cloned().collect();
            k.sort();
            k
        };
        assert_eq!(keys(&v), ["lan", "peers", "underlay"]);
        assert_eq!(keys(&v["lan"]), ["address", "interface"]);
        assert_eq!(keys(&v["peers"][0]), ["endpoint", "prefixes", "slot"]);
        for forbidden in [
            KEY_A,
            KEY_C,
            "gw-a",
            "gw-c",
            "private_key",
            "/etc/vpntunnel",
        ] {
            assert!(!json.contains(forbidden), "{forbidden}");
        }
        assert!(json.ends_with("}\n"));
    }
}
