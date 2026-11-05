//! `vptctl run`: the long-running controller.
//!
//! One session claims the gateway in this network namespace, loads the validated
//! configuration (VALIDATE and COMMIT), then runs the handshake state machine while
//! renewing the 5 second lease every second. If the lease or the kernel family is lost the
//! session ends and a new one starts after a pause: ownership is never assumed to survive.
//! Stopping the process releases the socket, which closes admission at once.

use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use thiserror::Error;

use crate::client::{Client, ClientError};
use crate::clock::BootClock;
use crate::config::{ConfigError, GatewayConfig, MAX_CONFIG_BYTES};
use crate::identity::{IdentityError, PrivateKey, PublicKey};
use crate::netlink_ids::{CMD_REKEY_GRANTED, CMD_REKEY_NEEDED};
use crate::session::{Controller, Entropy, Log};
use crate::usage::{Counters, Limits, Monitor, Silence};
use crate::vptunnel::{
    RekeyEvent, VptunnelError, encode_snapshot, parse_exchange_rx, parse_rekey_event,
};

const RENEW_EVERY: Duration = Duration::from_secs(1);
const TICK: Duration = Duration::from_millis(100);
const SESSION_PAUSE: Duration = Duration::from_secs(2);
/// The first configuration revision of a session; a reload commits the next one.
const REVISION: u64 = 1;
/// Kernel refusals that are permanent for the file content: the same bytes will be refused
/// again, so the version is dropped until the file changes. They leave the running
/// configuration in place. Anything not listed here or in `RELOAD_TRANSIENT` (a lost lease,
/// a stale owner, a vanished device) ends the session.
const RELOAD_REFUSALS: [i32; 4] = [
    7,  // E2BIG
    22, // EINVAL
    28, // ENOSPC
    90, // EMSGSIZE
];
/// Kernel refusals that clear by themselves: `EBUSY` is the retired-snapshot ceiling (two
/// snapshots still waiting for RCU reclamation) and `ENOMEM` is memory pressure. The running
/// configuration stays and the same file version is retried after `RELOAD_RETRY_DELAY`.
const RELOAD_TRANSIENT: [i32; 2] = [
    12, // ENOMEM
    16, // EBUSY
];
/// How long to wait before retrying a reload that hit a transient refusal; comfortably
/// longer than an RCU grace period.
const RELOAD_RETRY_DELAY: Duration = Duration::from_secs(3);
/// Total attempts (the first one included) at one file version before it is given up as
/// rejected. It is retried again only once the file changes.
const RELOAD_MAX_ATTEMPTS: u32 = 5;

/// How the kernel's refusal of a reload is treated.
#[derive(Debug, PartialEq, Eq)]
enum Refusal {
    /// The same content will always be refused.
    Permanent,
    /// Retry the same content later.
    Transient,
}

fn classify_refusal(errno: Option<i32>) -> Option<Refusal> {
    let n = errno?;
    if RELOAD_REFUSALS.contains(&n) {
        Some(Refusal::Permanent)
    } else if RELOAD_TRANSIENT.contains(&n) {
        Some(Refusal::Transient)
    } else {
        None
    }
}

#[derive(Debug, Error)]
pub enum DaemonError {
    #[error("configuration: {0}")]
    Config(#[from] ConfigError),
    #[error("identity: {0}")]
    Identity(#[from] IdentityError),
    #[error("{0}")]
    Client(#[from] ClientError),
    #[error("{0}")]
    Snapshot(#[from] VptunnelError),
    #[error("interface `{0}`: {1}")]
    Interface(String, std::io::Error),
    #[error("/dev/urandom: {0}")]
    Random(std::io::Error),
}

pub struct Options {
    pub config: PathBuf,
    pub request_budget: Duration,
    /// The gateway device created with `ip link add <name> type vpt1_ay1`.
    pub device: String,
    /// Stop after this many sessions (tests); `None` runs until killed.
    pub max_sessions: Option<u32>,
    /// Re-establish this long after each success; `None` uses the default (90 s).
    pub rekey_after: Option<Duration>,
    /// Re-key after this many bytes or records in either direction (default: see `usage`).
    pub rekey_after_bytes: Option<u64>,
    pub rekey_after_records: Option<u64>,
}

fn ifindex(name: &str) -> Result<u32, DaemonError> {
    let path = Path::new("/sys/class/net").join(name).join("ifindex");
    let text =
        std::fs::read_to_string(&path).map_err(|e| DaemonError::Interface(name.into(), e))?;
    text.trim().parse().map_err(|_| {
        DaemonError::Interface(
            name.into(),
            std::io::Error::new(std::io::ErrorKind::InvalidData, "bad ifindex"),
        )
    })
}

fn entropy() -> Result<Entropy, DaemonError> {
    let mut f = File::open("/dev/urandom").map_err(DaemonError::Random)?;
    Ok(Box::new(move |buf: &mut [u8]| {
        // A failing read of the OS random source must never fall back to anything weaker.
        f.read_exact(buf).expect("cannot read /dev/urandom");
    }))
}

fn logger(start: Instant) -> Log {
    Box::new(move |m| eprintln!("[{:7.2}s] vptctl: {m}", start.elapsed().as_secs_f64()))
}

/// Run sessions until killed (or `max_sessions`).
pub fn run(opts: &Options) -> Result<(), DaemonError> {
    let start = Instant::now();
    let mut say = logger(start);
    let mut sessions = 0u32;
    loop {
        sessions += 1;
        match session(opts, start) {
            Ok(()) => return Ok(()),
            Err(e @ (DaemonError::Config(_) | DaemonError::Identity(_))) => return Err(e),
            Err(e) => say(format!("session ended: {e}")),
        }
        if opts.max_sessions.is_some_and(|m| sessions >= m) {
            return Ok(());
        }
        std::thread::sleep(SESSION_PAUSE);
    }
}

fn session(opts: &Options, start: Instant) -> Result<(), DaemonError> {
    let bytes = read_config(&opts.config)?;
    let config = parse_config(&bytes)?;
    let key = PrivateKey::load(&config.gateway.private_key_file)?;
    config.check_local_identity(&key.public_key())?;
    let mut say = logger(start);

    let device = ifindex(&opts.device)?;
    let lan = ifindex(&config.gateway.lan_interface)?;
    let underlay = ifindex(&config.gateway.underlay_interface)?;
    let mut rng = entropy()?;
    let mut controller_id = [0u8; 16];
    rng(&mut controller_id);

    let mut client = Client::open(opts.request_budget, device, controller_id)?;
    client.claim()?;
    let snapshot = encode_snapshot(&config, lan, underlay, &key.public_key())?;
    let mut revision = REVISION;
    client.validate(revision, &snapshot)?;
    client.commit(revision)?;
    let status = client.status()?;
    let incarnations: Vec<_> = status
        .peers
        .iter()
        .map(|p| (p.slot, p.incarnation))
        .collect();
    say(format!(
        "gateway `{}` claimed, revision {revision} committed, {} peer(s)",
        config.gateway.name,
        incarnations.len()
    ));

    let public = key.public_key();
    let clock = BootClock::new();
    let mut controller =
        Controller::new(key, &config, &incarnations, rng, logger(start), clock.now());
    if let Some(d) = opts.rekey_after {
        controller.set_rekey_after(d);
    }
    let defaults = Limits::default();
    let mut usage = Monitor::new(Limits {
        bytes: opts.rekey_after_bytes.unwrap_or(defaults.bytes),
        records: opts.rekey_after_records.unwrap_or(defaults.records),
    });
    let mut watcher = Watcher::new(&opts.config, bytes);
    let mut silence = Silence::new();
    let mut last_renew = clock.now();
    loop {
        let now = clock.now();
        if now.duration_since(last_renew) >= RENEW_EVERY {
            client.renew(revision)?;
            last_renew = now;
            if let Some(new_bytes) = watcher.poll(now) {
                let ctx = Reload { public };
                match reload(
                    &mut client,
                    &mut revision,
                    &mut controller,
                    &ctx,
                    &new_bytes,
                    now,
                )? {
                    Reloaded::Applied(r) => {
                        watcher.applied(new_bytes);
                        say(format!(
                            "configuration reloaded as revision {revision}: kept {:?}, replaced {:?}, added {:?}, removed {:?}",
                            r.kept, r.replaced, r.added, r.removed
                        ));
                    }
                    Reloaded::Rejected(why) => {
                        watcher.rejected(new_bytes);
                        say(format!(
                            "configuration change rejected, still running revision {revision}: {why}"
                        ));
                    }
                    Reloaded::Transient(why) => match watcher.retry_later(new_bytes, now) {
                        Retry::Later { attempt } => say(format!(
                            "configuration change deferred, still running revision {revision} (attempt {attempt} of {RELOAD_MAX_ATTEMPTS}, retrying in {}s): {why}",
                            RELOAD_RETRY_DELAY.as_secs()
                        )),
                        Retry::GaveUp => say(format!(
                            "configuration change rejected after {RELOAD_MAX_ATTEMPTS} attempts, still running revision {revision}: {why}"
                        )),
                    },
                }
            }
            if let Some(c) = Counters::read(&opts.device)
                && usage.update(c, controller.counts().established)
            {
                let n = controller.request_rekey();
                say(format!("key usage reached a limit; re-keying {n} peer(s)"));
            }
            if let Some(c) = Counters::read(&opts.device)
                && silence.update(now, c, controller.counts().established)
            {
                let n = controller.request_rekey();
                if n > 0 {
                    say(format!(
                        "no records from the peer while sending; trying new keys with {n} peer(s)"
                    ));
                }
            }
        }
        controller.tick(&mut client, now);
        if let Some(event) = client.recv_event(TICK)? {
            match event.payload.first().copied() {
                Some(CMD_REKEY_GRANTED | CMD_REKEY_NEEDED) => {
                    match parse_rekey_event(&event.payload) {
                        Ok(RekeyEvent::Granted { slot, attempt, .. }) => {
                            controller.on_rekey_granted(&mut client, slot, attempt, clock.now());
                        }
                        Ok(RekeyEvent::Needed { slot, .. }) => {
                            if controller.on_rekey_needed(slot) {
                                say(format!("the kernel asked for a re-key of slot {slot}"));
                            }
                        }
                        Err(e) => say(format!("ignored a kernel event: {e}")),
                    }
                }
                _ => match parse_exchange_rx(&event.payload) {
                    Ok(ev) => controller.on_exchange(&mut client, ev, clock.now()),
                    Err(e) => say(format!("ignored a kernel event: {e}")),
                },
            }
        }
        if client.take_events_lost() {
            // INIT and RESPONSE are retransmitted, and REKEY_NEEDED is raised again by the kernel. A
            // lost REKEY_GRANTED is not: the controller repeats REKEY_REQUEST on a short schedule
            // inside the grant wait, and only then makes a fresh attempt (session.rs).
            say("kernel events were lost; retransmissions and repeated requests recover".into());
        }
    }
}

/// Read the configuration file once, bounded, so the bytes that were validated are the
/// bytes the watcher compares against later.
fn read_config(path: &Path) -> Result<Vec<u8>, ConfigError> {
    let io_err = |source| ConfigError::Io {
        path: path.display().to_string(),
        source,
    };
    let mut bytes = Vec::new();
    File::open(path)
        .map_err(io_err)?
        .take(MAX_CONFIG_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(io_err)?;
    if bytes.len() as u64 > MAX_CONFIG_BYTES {
        return Err(ConfigError::TooLarge);
    }
    Ok(bytes)
}

fn parse_config(bytes: &[u8]) -> Result<GatewayConfig, ConfigError> {
    let text =
        std::str::from_utf8(bytes).map_err(|_| ConfigError::Parse("not valid UTF-8".into()))?;
    GatewayConfig::parse(text)
}

/// Notices a changed configuration file. It compares the bytes, not a timestamp, so an
/// editor's write-then-rename and a file replaced with identical content both behave. A
/// version that failed permanently is not retried (or logged again) until the file changes
/// once more. A version that failed transiently is retried after a delay, a bounded number
/// of times, then treated as rejected; a changed file resets that state.
pub struct Watcher {
    path: PathBuf,
    applied: Vec<u8>,
    rejected: Option<Vec<u8>>,
    retry: Option<Pending>,
}

/// A file version waiting for a transient refusal to clear.
struct Pending {
    version: Vec<u8>,
    /// Attempts made so far, the first one included.
    attempts: u32,
    next_at: Instant,
}

/// What `Watcher::retry_later` decided.
#[derive(Debug, PartialEq, Eq)]
pub enum Retry {
    /// The same version will be offered again after the delay; `attempt` was the one that failed.
    Later { attempt: u32 },
    /// The attempts are used up; the version is now rejected.
    GaveUp,
}

impl Watcher {
    pub fn new(path: &Path, applied: Vec<u8>) -> Self {
        Self {
            path: path.to_owned(),
            applied,
            rejected: None,
            retry: None,
        }
    }

    /// The new content if the file now differs from what is running and from the last
    /// rejected version, and, for a version waiting out a transient refusal, only once its
    /// retry time has come. An unreadable file (mid-replace) is simply tried again next time.
    pub fn poll(&mut self, now: Instant) -> Option<Vec<u8>> {
        let bytes = match read_config(&self.path) {
            Ok(b) => b,
            // Too large is a change worth reporting; the parse step rejects it by size.
            Err(ConfigError::TooLarge) => vec![0; usize::try_from(MAX_CONFIG_BYTES).ok()? + 1],
            Err(_) => return None,
        };
        // A file that is no longer the version being retried resets the retry state.
        if self.retry.as_ref().is_some_and(|p| p.version != bytes) {
            self.retry = None;
        }
        if bytes == self.applied || self.rejected.as_deref() == Some(bytes.as_slice()) {
            return None;
        }
        if self.retry.as_ref().is_some_and(|p| now < p.next_at) {
            return None;
        }
        Some(bytes)
    }

    pub fn applied(&mut self, bytes: Vec<u8>) {
        self.applied = bytes;
        self.rejected = None;
        self.retry = None;
    }

    pub fn rejected(&mut self, bytes: Vec<u8>) {
        self.rejected = Some(bytes);
        self.retry = None;
    }

    /// The kernel refused `bytes` transiently at `now`: schedule another attempt, or give
    /// up (reject the version) once `RELOAD_MAX_ATTEMPTS` have been made.
    pub fn retry_later(&mut self, bytes: Vec<u8>, now: Instant) -> Retry {
        let attempts = match self.retry.take() {
            Some(p) if p.version == bytes => p.attempts + 1,
            _ => 1,
        };
        if attempts >= RELOAD_MAX_ATTEMPTS {
            self.rejected(bytes);
            return Retry::GaveUp;
        }
        self.retry = Some(Pending {
            version: bytes,
            attempts,
            next_at: now + RELOAD_RETRY_DELAY,
        });
        Retry::Later { attempt: attempts }
    }
}

struct Reload {
    /// The public half of the key the session runs with.
    public: PublicKey,
}

enum Reloaded {
    Applied(crate::session::Reconfigured),
    /// Nothing changed in the kernel or the controller; the running configuration stays.
    Rejected(String),
    /// The kernel refused for a reason that clears by itself; the running configuration
    /// stays and the same version should be retried later.
    Transient(String),
}

/// Apply a changed configuration file as the next revision of this session.
///
/// Everything that can be checked here is checked first; the kernel then validates the
/// snapshot again and commits it. Any refusal leaves the running configuration alone. Only
/// errors that mean the session itself is gone (lease, owner, device) are returned. `now`
/// is the caller's clock reading, so every deadline stays on the loop's clock.
fn reload(
    client: &mut Client,
    revision: &mut u64,
    controller: &mut Controller,
    ctx: &Reload,
    bytes: &[u8],
    now: Instant,
) -> Result<Reloaded, DaemonError> {
    let config = match parse_config(bytes) {
        Ok(c) => c,
        Err(e) => return Ok(Reloaded::Rejected(e.to_string())),
    };
    // The local identity cannot change under a running session: every peer would have to
    // be re-keyed against a different static key. That is a restart.
    match PrivateKey::load(&config.gateway.private_key_file) {
        Ok(k) if k.public_key() == ctx.public => {}
        Ok(_) => {
            return Ok(Reloaded::Rejected(
                "the gateway's own key changed; restart vptctl to use it".into(),
            ));
        }
        Err(e) => return Ok(Reloaded::Rejected(e.to_string())),
    }
    if let Err(e) = config.check_local_identity(&ctx.public) {
        return Ok(Reloaded::Rejected(e.to_string()));
    }
    let lan = match ifindex(&config.gateway.lan_interface) {
        Ok(i) => i,
        Err(e) => return Ok(Reloaded::Rejected(e.to_string())),
    };
    let underlay = match ifindex(&config.gateway.underlay_interface) {
        Ok(i) => i,
        Err(e) => return Ok(Reloaded::Rejected(e.to_string())),
    };
    let snapshot = match encode_snapshot(&config, lan, underlay, &ctx.public) {
        Ok(s) => s,
        Err(e) => return Ok(Reloaded::Rejected(e.to_string())),
    };

    let next = *revision + 1;
    for step in [Step::Validate, Step::Commit] {
        let result = match step {
            Step::Validate => client.validate(next, &snapshot),
            Step::Commit => client.commit(next),
        };
        match result {
            Ok(()) => {}
            Err(e) => match classify_refusal(e.errno()) {
                Some(Refusal::Permanent) => {
                    return Ok(Reloaded::Rejected(format!("the kernel refused it: {e}")));
                }
                Some(Refusal::Transient) => {
                    return Ok(Reloaded::Transient(format!("the kernel refused it: {e}")));
                }
                None => return Err(e.into()),
            },
        }
    }
    *revision = next;
    let status = client.status()?;
    let incarnations: Vec<_> = status
        .peers
        .iter()
        .map(|p| (p.slot, p.incarnation))
        .collect();
    Ok(Reloaded::Applied(controller.reconfigure(
        &config,
        &incarnations,
        now,
    )))
}

#[derive(Clone, Copy)]
enum Step {
    Validate,
    Commit,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t0() -> Instant {
        // One fixed reading per test process keeps the arithmetic below deterministic.
        static T: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
        *T.get_or_init(Instant::now)
    }

    fn temp_file(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("vpt-watch-{}-{tag}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("gw.toml")
    }

    #[test]
    fn an_unchanged_file_is_not_a_change() {
        let path = temp_file("same");
        std::fs::write(&path, b"one").unwrap();
        let mut w = Watcher::new(&path, b"one".to_vec());
        assert_eq!(w.poll(t0()), None);
        // Rewriting identical bytes (an editor's save, `touch`) is still no change.
        std::fs::write(&path, b"one").unwrap();
        assert_eq!(w.poll(t0()), None);
    }

    #[test]
    fn a_changed_file_is_reported_until_applied() {
        let path = temp_file("changed");
        std::fs::write(&path, b"one").unwrap();
        let mut w = Watcher::new(&path, b"one".to_vec());
        std::fs::write(&path, b"two").unwrap();
        assert_eq!(w.poll(t0()), Some(b"two".to_vec()));
        assert_eq!(
            w.poll(t0()),
            Some(b"two".to_vec()),
            "reported until the caller decides"
        );
        w.applied(b"two".to_vec());
        assert_eq!(w.poll(t0()), None);
        // Changing back is a change again.
        std::fs::write(&path, b"one").unwrap();
        assert_eq!(w.poll(t0()), Some(b"one".to_vec()));
    }

    #[test]
    fn a_rejected_version_is_not_retried_until_the_file_changes_again() {
        let path = temp_file("rejected");
        std::fs::write(&path, b"one").unwrap();
        let mut w = Watcher::new(&path, b"one".to_vec());
        std::fs::write(&path, b"bad").unwrap();
        let bad = w.poll(t0()).unwrap();
        w.rejected(bad);
        for _ in 0..3 {
            assert_eq!(w.poll(t0()), None, "no log or kernel traffic every second");
        }
        std::fs::write(&path, b"fixed").unwrap();
        assert_eq!(w.poll(t0()), Some(b"fixed".to_vec()));
        // Putting the running version back after a rejection is a change from "bad".
        w.applied(b"fixed".to_vec());
        std::fs::write(&path, b"bad").unwrap();
        assert_eq!(
            w.poll(t0()),
            Some(b"bad".to_vec()),
            "the rejection was forgotten once applied"
        );
    }

    #[test]
    fn a_missing_file_is_tried_again_later() {
        let path = temp_file("missing");
        let mut w = Watcher::new(&path, b"one".to_vec());
        assert_eq!(w.poll(t0()), None, "mid-replace: wait");
        std::fs::write(&path, b"two").unwrap();
        assert_eq!(w.poll(t0()), Some(b"two".to_vec()));
    }

    #[test]
    fn an_oversized_file_is_reported_and_then_rejected_by_the_parser() {
        let path = temp_file("big");
        std::fs::write(&path, b"one").unwrap();
        let mut w = Watcher::new(&path, b"one".to_vec());
        std::fs::write(
            &path,
            vec![b'#'; usize::try_from(MAX_CONFIG_BYTES).unwrap() + 100],
        )
        .unwrap();
        let got = w.poll(t0()).expect("a huge file is a change");
        assert!(got.len() as u64 > MAX_CONFIG_BYTES);
        assert!(matches!(parse_config(&got), Err(ConfigError::TooLarge)));
        w.rejected(got);
        assert_eq!(w.poll(t0()), None);
    }

    #[test]
    fn non_utf8_and_garbage_are_rejected_before_the_kernel_is_asked() {
        assert!(matches!(
            parse_config(&[0xff, 0xfe]),
            Err(ConfigError::Parse(_))
        ));
        assert!(parse_config(b"not = [toml").is_err());
    }

    #[test]
    fn ebusy_and_enomem_are_transient_and_the_rest_permanent() {
        for n in [16, 12] {
            assert_eq!(classify_refusal(Some(n)), Some(Refusal::Transient), "{n}");
        }
        for n in [22, 28, 7, 90] {
            assert_eq!(classify_refusal(Some(n)), Some(Refusal::Permanent), "{n}");
        }
        // A lost lease or stale owner is not a refusal at all: the session ends.
        assert_eq!(classify_refusal(Some(1)), None);
        assert_eq!(classify_refusal(None), None);
    }

    fn watcher_with_change(tag: &str) -> (Watcher, PathBuf) {
        let path = temp_file(tag);
        std::fs::write(&path, b"one").unwrap();
        let mut w = Watcher::new(&path, b"one".to_vec());
        std::fs::write(&path, b"two").unwrap();
        assert_eq!(w.poll(t0()), Some(b"two".to_vec()));
        (w, path)
    }

    #[test]
    fn a_transient_refusal_is_retried_after_the_delay_and_then_applies() {
        let (mut w, _) = watcher_with_change("transient-ok");
        let t = t0();
        assert_eq!(
            w.retry_later(b"two".to_vec(), t),
            Retry::Later { attempt: 1 }
        );
        assert_eq!(w.poll(t), None, "not before the delay");
        assert_eq!(
            w.poll(t + RELOAD_RETRY_DELAY - Duration::from_millis(1)),
            None
        );
        let later = t + RELOAD_RETRY_DELAY;
        assert_eq!(w.poll(later), Some(b"two".to_vec()), "the same version");
        w.applied(b"two".to_vec());
        assert_eq!(w.poll(later + RELOAD_RETRY_DELAY), None);
    }

    #[test]
    fn a_transient_refusal_that_never_clears_gives_up_and_is_not_retried_again() {
        let (mut w, path) = watcher_with_change("transient-exhausted");
        let mut t = t0();
        for attempt in 1..RELOAD_MAX_ATTEMPTS {
            assert_eq!(
                w.retry_later(b"two".to_vec(), t),
                Retry::Later { attempt },
                "attempt {attempt}"
            );
            t += RELOAD_RETRY_DELAY;
            assert_eq!(w.poll(t), Some(b"two".to_vec()));
        }
        assert_eq!(w.retry_later(b"two".to_vec(), t), Retry::GaveUp);
        for _ in 0..3 {
            t += RELOAD_RETRY_DELAY * 10;
            assert_eq!(w.poll(t), None, "rejected: no more attempts");
        }
        // Only a change to the file brings it back.
        std::fs::write(&path, b"three").unwrap();
        assert_eq!(w.poll(t), Some(b"three".to_vec()));
    }

    #[test]
    fn a_changed_file_during_the_retry_wait_resets_the_retry_state() {
        let (mut w, path) = watcher_with_change("transient-reset");
        let t = t0();
        for _ in 0..3 {
            let _ = w.retry_later(b"two".to_vec(), t);
        }
        std::fs::write(&path, b"three").unwrap();
        assert_eq!(
            w.poll(t),
            Some(b"three".to_vec()),
            "offered at once, without waiting for the old retry time"
        );
        assert_eq!(
            w.retry_later(b"three".to_vec(), t),
            Retry::Later { attempt: 1 },
            "a fresh budget, not 4 of 5"
        );
    }

    #[test]
    fn a_permanent_rejection_is_never_retried() {
        let (mut w, _) = watcher_with_change("permanent");
        w.rejected(b"two".to_vec());
        let mut t = t0();
        for _ in 0..10 {
            t += RELOAD_RETRY_DELAY * 10;
            assert_eq!(w.poll(t), None);
        }
    }
}
