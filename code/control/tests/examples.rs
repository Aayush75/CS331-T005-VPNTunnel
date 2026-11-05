//! The checked-in sample configurations must stay valid and mutually consistent.

use std::path::Path;

use vpt_control::config::{Direction, GatewayConfig, Role};

fn load(name: &str) -> GatewayConfig {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("examples")
        .join(name);
    GatewayConfig::load(&path).unwrap_or_else(|e| panic!("{name}: {e}"))
}

#[test]
fn samples_validate_and_mirror_each_other() {
    let a = load("gw-a.toml");
    let b = load("gw-b.toml");

    // Each side pins the other's endpoint and owns the other's LAN prefix.
    assert_eq!(a.peers[0].endpoint, b.gateway.underlay_address.addr());
    assert_eq!(b.peers[0].endpoint, a.gateway.underlay_address.addr());
    assert_eq!(a.peers[0].prefixes, vec![b.gateway.lan_address.subnet()]);
    assert_eq!(b.peers[0].prefixes, vec![a.gateway.lan_address.subnet()]);

    // Exactly one initiator per relationship, same path budget.
    assert_eq!(a.peers[0].role, Role::Initiator);
    assert_eq!(b.peers[0].role, Role::Responder);
    assert_eq!(a.peers[0].path_mtu, b.peers[0].path_mtu);

    // Same rules on both sides: A initiates, B admits.
    assert_eq!(a.rules.len(), b.rules.len());
    for (ra, rb) in a.rules.iter().zip(&b.rules) {
        assert_eq!(
            (ra.id, ra.source, ra.destination),
            (rb.id, rb.source, rb.destination)
        );
        assert_eq!((ra.protocol, ra.ports), (rb.protocol, rb.ports));
        assert_eq!(ra.direction, Direction::LocalToPeer);
        assert_eq!(rb.direction, Direction::PeerToLocal);
    }
}

#[test]
fn sample_policy_denies_a_to_b2() {
    let a = load("gw-a.toml");
    let a_host = "10.77.1.10".parse().unwrap();
    let b2 = "10.77.2.11".parse().unwrap();
    assert!(
        !a.rules
            .iter()
            .any(|r| r.source.contains_addr(a_host) && r.destination.contains_addr(b2))
    );
}

// The two-Pi demo templates in infra/gateway, loaded as checked in. Interface names and keys are
// the only substitutions, done the way tests/netns/demo_reachability.py make_config does it;
// everything else (addresses, roles, rules, prefixes, MTU) is validated exactly as committed.

const TEST_KEY_1: &str = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
const TEST_KEY_2: &str = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";

fn demo_text(template: &str, site: &str, peer_pub: &str) -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../infra/gateway")
        .join(template);
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{template}: {e}"));
    let mut out = Vec::new();
    for line in text.lines() {
        let line = if line.starts_with("public_key = ") {
            format!("public_key = \"{peer_pub}\"")
        } else {
            line.replace("\"wlan0\"", &format!("\"veth_gw{site}_lan\""))
                .replace("\"eth0\"", &format!("\"veth_gw{site}_out\""))
        };
        out.push(line);
    }
    out.join("\n")
}

fn demo(template: &str, site: &str, peer_pub: &str) -> GatewayConfig {
    GatewayConfig::parse(&demo_text(template, site, peer_pub))
        .unwrap_or_else(|e| panic!("{template}: {e}"))
}

fn demo_pair() -> (GatewayConfig, GatewayConfig) {
    (
        demo("demo-pi-a.toml", "a", TEST_KEY_2),
        demo("demo-pi-b.toml", "b", TEST_KEY_1),
    )
}

fn assert_demo_mirrors(a: &GatewayConfig, b: &GatewayConfig) {
    assert_eq!((a.peers.len(), b.peers.len()), (1, 1));
    let (pa, pb) = (&a.peers[0], &b.peers[0]);

    // Each side's endpoint is the other's underlay address.
    assert_eq!(pa.endpoint, b.gateway.underlay_address.addr());
    assert_eq!(pb.endpoint, a.gateway.underlay_address.addr());

    // Opposite roles: one initiator, one responder.
    assert_eq!((pa.role, pb.role), (Role::Initiator, Role::Responder));

    // Equal path MTU, and it is the 1500 the demo documents.
    assert_eq!(pa.path_mtu, pb.path_mtu);
    assert_eq!(pa.path_mtu, 1500);

    // Each side's peer prefixes are exactly the other's LAN.
    assert_eq!(pa.prefixes, vec![b.gateway.lan_address.subnet()]);
    assert_eq!(pb.prefixes, vec![a.gateway.lan_address.subnet()]);

    // Six rules, identical selectors, opposite directions on the two sides.
    assert_eq!((a.rules.len(), b.rules.len()), (6, 6));
    for (ra, rb) in a.rules.iter().zip(&b.rules) {
        assert_eq!(
            (ra.id, ra.source, ra.destination, ra.protocol, ra.ports),
            (rb.id, rb.source, rb.destination, rb.protocol, rb.ports)
        );
        assert_ne!(ra.direction, rb.direction);
    }
    // Ids 1,3,5 go A to B and 2,4,6 go B to A.
    for r in &a.rules {
        let want = if r.id % 2 == 1 {
            Direction::LocalToPeer
        } else {
            Direction::PeerToLocal
        };
        assert_eq!(r.direction, want, "rule {} on pi-a", r.id);
    }
}

#[test]
fn demo_templates_validate_and_mirror_each_other() {
    let (a, b) = demo_pair();
    assert_demo_mirrors(&a, &b);
}

#[test]
fn demo_mirror_check_catches_a_broken_invariant() {
    let (a, _) = demo_pair();
    // A copy of pi-b whose endpoint no longer points at pi-a must fail the mirror check.
    let text = demo_text("demo-pi-b.toml", "b", TEST_KEY_1).replace(
        "endpoint = \"172.31.250.11\"",
        "endpoint = \"172.31.250.99\"",
    );
    assert_ne!(text, demo_text("demo-pi-b.toml", "b", TEST_KEY_1));
    let broken = GatewayConfig::parse(&text).unwrap();
    let r = std::panic::catch_unwind(|| assert_demo_mirrors(&a, &broken));
    assert!(r.is_err(), "a broken endpoint must be detected");
}

#[test]
fn demo_network_summary_has_the_fields_the_scripts_read() {
    use vpt_control::network::Network;
    let (a, b) = demo_pair();
    for (cfg, other, lan_if, lan_addr) in [
        (&a, &b, "veth_gwa_lan", "10.77.1.1/24"),
        (&b, &a, "veth_gwb_lan", "10.77.2.1/24"),
    ] {
        let v: serde_json::Value =
            serde_json::from_str(&Network::from_config(cfg).to_json()).unwrap();
        // vpt-gateway.sh: ["lan"]["interface"] and every peer's ["prefixes"].
        assert_eq!(v["lan"]["interface"], lan_if);
        // vpt-ap.sh: ["lan"]["address"] as an interface address with prefix length.
        assert_eq!(v["lan"]["address"], lan_addr);
        assert_eq!(
            v["underlay"]["address"],
            cfg.gateway.underlay_address.to_string()
        );
        let peers = v["peers"].as_array().unwrap();
        assert_eq!(peers.len(), 1);
        assert_eq!(
            peers[0]["endpoint"],
            other.gateway.underlay_address.addr().to_string()
        );
        let prefixes: Vec<&str> = peers[0]["prefixes"]
            .as_array()
            .unwrap()
            .iter()
            .map(|p| p.as_str().unwrap())
            .collect();
        assert_eq!(
            prefixes,
            vec![other.gateway.lan_address.subnet().to_string()]
        );
    }
}
