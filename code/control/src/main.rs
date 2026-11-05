use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};

use vpt_control::config::{Direction, GatewayConfig, Protocol, VPT1_PORT};
use vpt_control::identity::{PrivateKey, PublicKey};
use vpt_control::network::Network;
use vpt_control::vptunnel::encode_snapshot;

/// VPT1 gateway identity and configuration tool.
#[derive(Parser)]
#[command(version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Generate a new owner-only private identity file; never overwrites.
    Genkey {
        #[arg(long)]
        out: PathBuf,
    },
    /// Print the public key and fingerprint of a private identity file.
    Pubkey {
        #[arg(long)]
        key: PathBuf,
    },
    /// Print the BLAKE2s-256 fingerprint of a public key.
    Fingerprint { public_key: String },
    /// Validate a gateway configuration and print a summary without secrets.
    CheckConfig {
        file: PathBuf,
        /// Also load the private identity file it names.
        #[arg(long)]
        with_key: bool,
    },
    /// Print the network summary (JSON) of a valid configuration: links, peer
    /// slots, endpoints and prefixes, for `infra/gateway/vpt-gateway.sh`.
    /// Installs nothing.
    Network { file: PathBuf },
    /// Print the kernel SNAPSHOT payload (hex) the controller would send for this
    /// configuration, exactly as `vptctl run` encodes it. Validates the configuration
    /// and the gateway key first; talks to no kernel, but the private key file named
    /// in the configuration must exist and be readable, as for `run`. For tests and
    /// diagnostics.
    Snapshot {
        file: PathBuf,
        /// Interface index to encode for the LAN interface.
        #[arg(long)]
        lan_ifindex: u32,
        /// Interface index to encode for the underlay interface.
        #[arg(long)]
        underlay_ifindex: u32,
    },
    /// Run the controller: claim the gateway in this network namespace, load the
    /// configuration into the kernel, run the handshake with each peer and keep the
    /// 5 second lease. Stopping the process closes admission. Needs CAP_NET_ADMIN.
    #[cfg(target_os = "linux")]
    Run {
        file: PathBuf,
        /// Gateway device created with `ip link add vpn0 type vpt1_ay1`.
        #[arg(long, default_value = "vpn0")]
        device: String,
        /// Total time allowed for one Netlink request, in milliseconds.
        #[arg(long, default_value_t = 1000)]
        request_budget_ms: u64,
        /// Seconds after a successful exchange before the initiator starts a new one
        /// (default 90, below the kernel's 120 s soft key age). For tests.
        #[arg(long, hide = true)]
        rekey_after_secs: Option<u64>,
        /// Re-key after this much traffic in either direction (default 300 MiB). For tests.
        #[arg(long, hide = true)]
        rekey_after_bytes: Option<u64>,
        /// Re-key after this many records in either direction (default 300000). For tests.
        #[arg(long, hide = true)]
        rekey_after_records: Option<u64>,
    },
}

fn print_identity(pk: &PublicKey) {
    println!("public_key  {pk}");
    println!("fingerprint {}", pk.fingerprint());
}

fn run(cli: Cli) -> Result<(), Box<dyn std::error::Error>> {
    match cli.command {
        Command::Genkey { out } => {
            let key = PrivateKey::generate()?;
            key.save_new(&out)?;
            print_identity(&key.public_key());
        }
        Command::Pubkey { key } => print_identity(&PrivateKey::load(&key)?.public_key()),
        Command::Fingerprint { public_key } => {
            println!("{}", public_key.parse::<PublicKey>()?.fingerprint());
        }
        Command::CheckConfig { file, with_key } => {
            let config = GatewayConfig::load(&file)?;
            if with_key {
                let local = PrivateKey::load(&config.gateway.private_key_file)?.public_key();
                config.check_local_identity(&local)?;
                println!("identity    {}", local.fingerprint());
            }
            print_summary(&config);
        }
        Command::Network { file } => {
            let config = GatewayConfig::load(&file)?;
            print!("{}", Network::from_config(&config).to_json());
        }
        Command::Snapshot {
            file,
            lan_ifindex,
            underlay_ifindex,
        } => {
            let config = GatewayConfig::load(&file)?;
            let local = PrivateKey::load(&config.gateway.private_key_file)?.public_key();
            config.check_local_identity(&local)?;
            let payload = encode_snapshot(&config, lan_ifindex, underlay_ifindex, &local)?;
            println!("{}", hex::encode(payload));
        }
        #[cfg(target_os = "linux")]
        Command::Run {
            file,
            device,
            request_budget_ms,
            rekey_after_secs,
            rekey_after_bytes,
            rekey_after_records,
        } => vpt_control::daemon::run(&vpt_control::daemon::Options {
            config: file,
            request_budget: std::time::Duration::from_millis(request_budget_ms),
            device,
            max_sessions: None,
            rekey_after: rekey_after_secs.map(std::time::Duration::from_secs),
            rekey_after_bytes,
            rekey_after_records,
        })?,
    }
    Ok(())
}

fn print_summary(c: &GatewayConfig) {
    let g = &c.gateway;
    println!("gateway     {}", g.name);
    println!("lan         {} on {}", g.lan_address, g.lan_interface);
    println!(
        "underlay    {}:{VPT1_PORT} on {}",
        g.underlay_address, g.underlay_interface
    );
    for p in &c.peers {
        let prefixes: Vec<String> = p.prefixes.iter().map(ToString::to_string).collect();
        println!(
            "peer        {} slot {} {:?} {}:{VPT1_PORT} pmtu {} [{}] fp {}",
            p.name,
            p.slot,
            p.role,
            p.endpoint,
            p.path_mtu,
            prefixes.join(", "),
            p.public_key.fingerprint()
        );
    }
    for r in &c.rules {
        let arrow = match r.direction {
            Direction::LocalToPeer => "->",
            Direction::PeerToLocal => "<-",
        };
        let what = match (r.protocol, r.ports) {
            (Protocol::IcmpEcho, _) => "icmp-echo".to_owned(),
            (p, Some(range)) if range.first == range.last => format!("{p:?} {}", range.first),
            (p, Some(range)) => format!("{p:?} {}-{}", range.first, range.last),
            (p, None) => format!("{p:?}"),
        };
        println!(
            "rule {:<6} {arrow} {} {} -> {} {}",
            r.id, c.peers[r.peer].name, r.source, r.destination, what
        );
    }
    println!("ok          default deny; {} allow rule(s)", c.rules.len());
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("vptctl: {e}");
            ExitCode::FAILURE
        }
    }
}
