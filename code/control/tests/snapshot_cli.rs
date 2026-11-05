//! `vptctl snapshot` prints exactly the bytes the controller sends to the kernel, and
//! refuses what `check-config --with-key` refuses.

use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use vpt_control::config::GatewayConfig;
use vpt_control::identity::PrivateKey;
use vpt_control::vptunnel::encode_snapshot;

fn vptctl(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_vptctl"))
        .args(args)
        .output()
        .expect("run vptctl")
}

fn scratch(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("vpt-snapshot-{}-{tag}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// `examples/gw-a.toml` with its key path pointing at a freshly generated key.
fn sample_config(dir: &Path) -> PathBuf {
    let key = dir.join("gw.key");
    let out = vptctl(&["genkey", "--out", key.to_str().unwrap()]);
    assert!(out.status.success());
    let sample = Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/gw-a.toml");
    let text = std::fs::read_to_string(sample)
        .unwrap()
        .replace("/etc/vpntunnel/gw-a.key", key.to_str().unwrap());
    let path = dir.join("gw.toml");
    std::fs::write(&path, text).unwrap();
    path
}

#[test]
fn it_prints_the_exact_snapshot_the_controller_encodes() {
    let dir = scratch("exact");
    let cfg = sample_config(&dir);
    let out = vptctl(&[
        "snapshot",
        cfg.to_str().unwrap(),
        "--lan-ifindex",
        "7",
        "--underlay-ifindex",
        "9",
    ]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let printed = hex::decode(String::from_utf8(out.stdout).unwrap().trim()).unwrap();

    let config = GatewayConfig::load(&cfg).unwrap();
    let local = PrivateKey::load(&config.gateway.private_key_file)
        .unwrap()
        .public_key();
    assert_eq!(printed, encode_snapshot(&config, 7, 9, &local).unwrap());
}

#[test]
fn it_refuses_a_missing_file_a_bad_config_and_a_foreign_key() {
    let dir = scratch("refuse");
    let cfg = sample_config(&dir);
    let args = |file: &Path| {
        vptctl(&[
            "snapshot",
            file.to_str().unwrap(),
            "--lan-ifindex",
            "1",
            "--underlay-ifindex",
            "2",
        ])
    };
    assert!(!args(&dir.join("absent.toml")).status.success());

    let broken = dir.join("broken.toml");
    std::fs::write(
        &broken,
        std::fs::read_to_string(&cfg)
            .unwrap()
            .replace("path_mtu = 1500", "path_mtu = 1279"),
    )
    .unwrap();
    let out = args(&broken);
    assert!(!out.status.success() && out.stdout.is_empty());

    // The configuration names a key file that does not exist.
    let nokey = dir.join("nokey.toml");
    std::fs::write(
        &nokey,
        std::fs::read_to_string(&cfg)
            .unwrap()
            .replace("gw.key", "missing.key"),
    )
    .unwrap();
    assert!(!args(&nokey).status.success());
}

/// The controller half of the fixed must-accept list in `tests/netns/rj4_validate_corpus.py`: each
/// configuration at a limit (path MTU 1280 and 1500, 1 and 4 prefixes, exactly 1 and exactly 128 rules,
/// mixed protocols and ports, four peers) must be accepted. The kernel half runs in the VM suite.
#[test]
fn it_accepts_every_fixed_must_accept_configuration() {
    let dir = scratch("must-accept");
    let key = dir.join("gw.key");
    assert!(
        vptctl(&["genkey", "--out", key.to_str().unwrap()])
            .status
            .success()
    );
    let snapshot = |file: &Path| {
        vptctl(&[
            "snapshot",
            file.to_str().unwrap(),
            "--lan-ifindex",
            "1",
            "--underlay-ifindex",
            "2",
        ])
    };
    let vectors = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/vectors/must_accept");
    let mut accepted = 0;
    for entry in std::fs::read_dir(&vectors).unwrap() {
        let path = entry.unwrap().path();
        let text = std::fs::read_to_string(&path)
            .unwrap()
            .replace("@KEY@", key.to_str().unwrap());
        let cfg = dir.join(path.file_name().unwrap());
        std::fs::write(&cfg, &text).unwrap();
        let out = snapshot(&cfg);
        assert!(
            out.status.success(),
            "{} refused: {}",
            path.display(),
            String::from_utf8_lossy(&out.stderr)
        );
        accepted += 1;

        // One rule more than the 128-rule configuration holds is refused: the list is not vacuous.
        if path.file_name().unwrap() == "exactly-128-rules.toml" {
            let over = dir.join("over.toml");
            let extra = "\n[[rule]]\nid = 129\npeer = \"peer0\"\nsource = \"10.77.1.10/32\"\n\
                         destination = \"10.77.2.10/32\"\nprotocol = \"tcp\"\nports = \"9999\"\n";
            std::fs::write(&over, text + extra).unwrap();
            assert!(!snapshot(&over).status.success(), "129 rules were accepted");
        }
    }
    assert!(accepted >= 10, "only {accepted} fixed configurations found");
}
