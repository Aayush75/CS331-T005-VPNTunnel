//! The handshake state machine of the long-running controller.
//!
//! For every configured peer the controller either initiates the Noise IK exchange
//! (`role = "initiator"`) or answers it (`role = "responder"`), and hands the resulting
//! directional keys to the kernel once. All I/O goes through [`Port`], so the whole
//! machine (retransmission, duplicates, forgeries, timeouts, backoff, re-establishment)
//! is tested without a kernel. The kernel moves the bytes and keeps the counters; this
//! code verifies the exchange and never sees a data packet.
//!
//! Timing follows TECHNICAL_DESIGN.md: INIT is retransmitted at 0.5, 1, 2 and 4 second
//! intervals within a 10 second attempt; a failed attempt backs off 1, 2, 4, 8, 16, then
//! 30 seconds; the responder answers a byte-identical duplicate INIT from its cache for 20
//! seconds without advancing Noise again.
//!
//! Re-keying keeps the flows when it can: before the kernel's generation reaches its 120 second
//! soft age the initiator asks for a continuity grant (REKEY_REQUEST), and the grant (REKEY_GRANTED)
//! releases a mode 1 INIT that names the running session as its parent; the kernel then runs the
//! activation (`docs/AY4_ACTIVATION.md`). When no grant comes, or the kernel refuses the parent, the
//! controller falls back to a fresh (mode 0) attempt, which replaces the session and clears the flows.

use std::time::{Duration, Instant};

use thiserror::Error;

use crate::config::{GatewayConfig, Role};
use crate::exchange::{
    ExchangeError, Initiator, ResponderParams, ResponseCache, SessionKeys, respond,
};
use crate::identity::{PrivateKey, PublicKey};
use crate::vptunnel::ExchangeRx;
use crate::wire::{
    ATTEMPT_ID_LEN, HASH_LEN, HandshakePayload, INCARNATION_LEN, Mode, parse_handshake,
};

/// One attempt's overall deadline, and the INIT retransmission intervals inside it.
pub const ATTEMPT_DEADLINE: Duration = Duration::from_secs(10);
const RETRANSMIT: [Duration; 4] = [
    Duration::from_millis(500),
    Duration::from_secs(1),
    Duration::from_secs(2),
    Duration::from_secs(4),
];
const BACKOFF: [Duration; 6] = [
    Duration::from_secs(1),
    Duration::from_secs(2),
    Duration::from_secs(4),
    Duration::from_secs(8),
    Duration::from_secs(16),
    Duration::from_secs(30),
];
/// How long an initiator waits for the responder's continuity grant before it gives up on keeping
/// the flows and makes a fresh attempt instead (the kernel retries REKEY_START inside this).
pub const GRANT_WAIT: Duration = Duration::from_secs(5);
/// REKEY_GRANTED is raised once, so a lost event would cost the flows. Inside the wait the controller
/// repeats REKEY_REQUEST for the same attempt after these delays (1 s, then 2 s later): the kernel
/// takes a repeat for its live attempt, re-sends REKEY_START, and the responder answers it while its
/// grant lasts, which raises REKEY_GRANTED again.
const GRANT_REPEAT: [Duration; 2] = [Duration::from_secs(1), Duration::from_secs(2)];
/// errno the kernel returns when STAGE_GENERATION refuses a mode 1 attempt (no live grant, or a parent
/// that is not its current generation's hash).
const EPERM: i32 = 1;
/// How long the responder keeps the exact RESPONSE for a duplicate INIT.
pub const CACHE_LIFETIME: Duration = Duration::from_secs(20);
/// The initiator starts a new exchange this long after the last success, before the
/// kernel's 120 s soft key age.
pub const REKEY_AFTER: Duration = Duration::from_secs(90);

#[derive(Debug, Error)]
#[error("{what}")]
pub struct PortError {
    pub what: String,
    /// The kernel's errno when the kernel rejected the request.
    pub errno: Option<i32>,
}

/// The kernel operations the exchange needs.
pub trait Port {
    /// Ask the kernel for this attempt's ID and receive locator. The kernel chooses both; a
    /// responder passes the attempt ID of the INIT it was handed (`given`) and gets only a locator.
    fn begin_attempt(
        &mut self,
        slot: u8,
        kernel_incarnation: &[u8; INCARNATION_LEN],
        given: Option<&[u8; ATTEMPT_ID_LEN]>,
    ) -> Result<([u8; ATTEMPT_ID_LEN], u64), PortError>;
    /// Initiator only: ask the responder, through the kernel, for a continuity grant for the attempt
    /// `attempt` the kernel just issued. The grant comes back as a REKEY_GRANTED event.
    fn rekey_request(
        &mut self,
        slot: u8,
        kernel_incarnation: &[u8; INCARNATION_LEN],
        attempt: &[u8; ATTEMPT_ID_LEN],
    ) -> Result<(), PortError>;
    /// Send one whole INIT or RESPONSE to the peer's configured endpoint.
    fn exchange_tx(&mut self, slot: u8, bytes: &[u8]) -> Result<(), PortError>;
    /// Install the directional keys and locators of a completed attempt, once.
    fn stage(
        &mut self,
        slot: u8,
        kernel_incarnation: &[u8; INCARNATION_LEN],
        keys: &SessionKeys,
    ) -> Result<(), PortError>;
    /// The kernel's current incarnation for a slot (after the kernel replaced the peer).
    fn kernel_incarnation(&mut self, slot: u8) -> Result<[u8; INCARNATION_LEN], PortError>;
}

pub type Entropy = Box<dyn FnMut(&mut [u8]) + Send>;
pub type Log = Box<dyn FnMut(String) + Send>;

struct Pending {
    initiator: Initiator,
    started: Instant,
    next_retry: Instant,
    retries: usize,
}

/// A continuity-keeping re-key waiting for the responder's grant: the kernel issued the attempt and
/// asked for the grant; the mode 1 INIT goes out when REKEY_GRANTED arrives.
struct Awaiting {
    attempt_id: [u8; ATTEMPT_ID_LEN],
    initiator_locator: u64,
    parent: [u8; HASH_LEN],
    /// When BEGIN_ATTEMPT was called: the kernel's attempt window starts here, not at the INIT.
    started: Instant,
    /// When REKEY_REQUEST is repeated next, and how many repeats are done.
    next_repeat: Option<Instant>,
    repeats: usize,
}

enum State {
    Idle { not_before: Instant },
    AwaitGrant(Box<Awaiting>),
    Pending(Box<Pending>),
    Established { since: Instant },
}

struct Cached {
    cache: ResponseCache,
    at: Instant,
}

struct PeerCtl {
    name: String,
    slot: u8,
    public: PublicKey,
    role: Role,
    path_budget: u16,
    kernel_incarnation: [u8; INCARNATION_LEN],
    state: State,
    failures: usize,
    cached: Option<Cached>,
    /// A new exchange was asked for (key usage); the initiator starts it at the next tick.
    rekey_requested: bool,
    /// The full handshake hash of the last session this controller staged (not necessarily the one
    /// the kernel has committed): the parent a continuity-keeping re-key names, and what a responder
    /// checks an INIT's parent against. The kernel is the authority and refuses a parent that is not
    /// its current hash (-EPERM); that refusal clears this and falls back to a fresh attempt.
    session_hash: Option<[u8; HASH_LEN]>,
}

/// What happened, for the caller's logs and tests.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Counts {
    pub established: u64,
    pub attempts_started: u64,
    pub attempts_failed: u64,
    pub rejected: u64,
    pub duplicates_answered: u64,
}

/// What a reconfiguration did to each slot, for the caller's log and tests.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct Reconfigured {
    /// Session and timers continue untouched (a policy-only change, or no change).
    pub kept: Vec<u8>,
    /// The kernel replaced the peer; a fresh exchange starts.
    pub replaced: Vec<u8>,
    /// A new slot; an exchange starts.
    pub added: Vec<u8>,
    /// The slot is gone from the configuration; its state is dropped.
    pub removed: Vec<u8>,
}

pub struct Controller {
    key: PrivateKey,
    incarnation: [u8; INCARNATION_LEN],
    peers: Vec<PeerCtl>,
    log: Log,
    counts: Counts,
    rekey_after: Duration,
}

fn is_zero(b: &[u8]) -> bool {
    b.iter().all(|x| *x == 0)
}

impl Controller {
    /// `kernel_incarnations` maps each configured slot to the kernel's incarnation of
    /// that peer (from STATUS after the commit).
    pub fn new(
        key: PrivateKey,
        config: &GatewayConfig,
        kernel_incarnations: &[(u8, [u8; INCARNATION_LEN])],
        mut rng: Entropy,
        log: Log,
        now: Instant,
    ) -> Self {
        let mut incarnation = [0u8; INCARNATION_LEN];
        while is_zero(&incarnation) {
            rng(&mut incarnation);
        }
        let peers = config
            .peers
            .iter()
            .map(|p| PeerCtl {
                name: p.name.clone(),
                slot: p.slot,
                public: p.public_key,
                role: p.role,
                path_budget: p.path_mtu,
                kernel_incarnation: kernel_incarnations
                    .iter()
                    .find(|(s, _)| *s == p.slot)
                    .map_or([0; INCARNATION_LEN], |(_, i)| *i),
                state: State::Idle { not_before: now },
                failures: 0,
                cached: None,
                rekey_requested: false,
                session_hash: None,
            })
            .collect();
        Self {
            key,
            incarnation,
            peers,
            log,
            counts: Counts {
                established: 0,
                attempts_started: 0,
                attempts_failed: 0,
                rejected: 0,
                duplicates_answered: 0,
            },
            rekey_after: REKEY_AFTER,
        }
    }

    /// Change the re-establishment interval (tests; the default is [`REKEY_AFTER`]).
    pub fn set_rekey_after(&mut self, d: Duration) {
        self.rekey_after = d;
    }

    pub fn counts(&self) -> Counts {
        self.counts
    }

    /// Is the exchange with this slot complete and still current?
    pub fn is_established(&self, slot: u8) -> bool {
        self.peers.iter().any(|p| {
            p.slot == slot && matches!(p.state, State::Established { .. } | State::AwaitGrant(_))
        })
    }

    fn say(&mut self, msg: String) {
        (self.log)(msg);
    }

    /// The kernel issues the attempt ID and the receive locator. A kernel that replaced the peer
    /// (a disruptive change) is learned once, as for staging.
    fn begin_attempt<P: Port>(
        &mut self,
        i: usize,
        port: &mut P,
        given: Option<&[u8; ATTEMPT_ID_LEN]>,
    ) -> Result<([u8; ATTEMPT_ID_LEN], u64), PortError> {
        let slot = self.peers[i].slot;
        let mut r = port.begin_attempt(slot, &self.peers[i].kernel_incarnation, given);
        if matches!(&r, Err(e) if e.errno == Some(116)) {
            let inc = port.kernel_incarnation(slot)?;
            self.peers[i].kernel_incarnation = inc;
            r = port.begin_attempt(slot, &inc, given);
        }
        r
    }

    /// Time-driven work: start attempts, retransmit INIT, expire attempts, re-establish.
    pub fn tick<P: Port>(&mut self, port: &mut P, now: Instant) {
        for i in 0..self.peers.len() {
            if self.peers[i].role != Role::Initiator {
                continue;
            }
            enum Action {
                None,
                Start,
                StartGranted,
                GrantTimeout,
                RepeatGrantRequest,
                Retransmit,
                Expire,
            }
            let action = match &self.peers[i].state {
                State::Idle { not_before } if now >= *not_before => Action::Start,
                State::Idle { .. } => Action::None,
                State::AwaitGrant(a) if now.duration_since(a.started) >= GRANT_WAIT => {
                    Action::GrantTimeout
                }
                State::AwaitGrant(a) if a.next_repeat.is_some_and(|t| now >= t) => {
                    Action::RepeatGrantRequest
                }
                State::AwaitGrant(_) => Action::None,
                State::Pending(p) if now.duration_since(p.started) >= ATTEMPT_DEADLINE => {
                    Action::Expire
                }
                State::Pending(p) if now >= p.next_retry => Action::Retransmit,
                State::Pending(_) => Action::None,
                State::Established { since }
                    if self.peers[i].rekey_requested
                        || now.duration_since(*since) >= self.rekey_after =>
                {
                    Action::StartGranted
                }
                State::Established { .. } => Action::None,
            };
            match action {
                Action::None => {}
                Action::Start => self.start_attempt(i, port, now, false),
                Action::StartGranted => self.start_attempt(i, port, now, true),
                Action::GrantTimeout => {
                    // No continuity grant in time: keep no flows, but do re-key.
                    let name = self.peers[i].name.clone();
                    self.say(format!(
                        "no continuity grant from {name}; making a fresh attempt instead"
                    ));
                    self.start_attempt(i, port, now, false);
                }
                Action::RepeatGrantRequest => self.repeat_grant_request(i, port, now),
                Action::Retransmit => self.retransmit(i, port, now),
                Action::Expire => {
                    self.counts.attempts_failed += 1;
                    let f = self.peers[i].failures;
                    self.peers[i].failures += 1;
                    let wait = BACKOFF[f.min(BACKOFF.len() - 1)];
                    let name = self.peers[i].name.clone();
                    self.say(format!(
                        "attempt to {name} timed out; retrying in {}s",
                        wait.as_secs()
                    ));
                    self.peers[i].state = State::Idle {
                        not_before: now + wait,
                    };
                }
            }
        }
    }

    /// Ask every established initiator peer for a new exchange now (key usage reached a limit).
    /// Responders cannot start one. Returns how many peers will start.
    pub fn request_rekey(&mut self) -> usize {
        let mut n = 0;
        for p in &mut self.peers {
            if p.role == Role::Initiator && matches!(p.state, State::Established { .. }) {
                p.rekey_requested = true;
                n += 1;
            }
        }
        n
    }

    /// The kernel asked for a re-key of one peer (keys near their limits, the responder asked, or
    /// the peer closed). Only an established initiator can start it; false otherwise.
    pub fn on_rekey_needed(&mut self, slot: u8) -> bool {
        match self.peers.iter_mut().find(|p| p.slot == slot) {
            Some(p)
                if p.role == Role::Initiator && matches!(p.state, State::Established { .. }) =>
            {
                p.rekey_requested = true;
                true
            }
            _ => false,
        }
    }

    /// The responder granted continuity for the attempt this initiator asked about: the mode 1
    /// INIT goes out now, naming the session being continued as its parent.
    pub fn on_rekey_granted<P: Port>(
        &mut self,
        port: &mut P,
        slot: u8,
        attempt: [u8; ATTEMPT_ID_LEN],
        now: Instant,
    ) {
        let Some(i) = self.peers.iter().position(|p| p.slot == slot) else {
            return;
        };
        let State::AwaitGrant(a) = &self.peers[i].state else {
            return; // Late or repeated: nothing is waiting for it.
        };
        if a.attempt_id != attempt {
            return;
        }
        let (attempt_id, initiator_locator, parent, began) =
            (a.attempt_id, a.initiator_locator, a.parent, a.started);
        // The kernel's attempt window runs from BEGIN_ATTEMPT, so the exchange is timed from there.
        self.send_init(
            i,
            port,
            now,
            began,
            Mode::Rekey,
            attempt_id,
            initiator_locator,
            parent,
        );
    }

    /// Adopt a configuration the kernel has just committed as a later revision.
    ///
    /// `kernel_incarnations` is the kernel's STATUS after that commit. The kernel keeps a
    /// peer's session across a commit only when nothing about the peer changed (role,
    /// path MTU, endpoint, key, prefixes), so a slot whose incarnation is unchanged keeps
    /// its exchange state and its running timers. Every other slot gets a fresh peer here
    /// and a new exchange: the controller does not repeat the kernel's classification, it
    /// follows the incarnation. A removed peer's state, including its cached RESPONSE, is
    /// dropped.
    pub fn reconfigure(
        &mut self,
        config: &GatewayConfig,
        kernel_incarnations: &[(u8, [u8; INCARNATION_LEN])],
        now: Instant,
    ) -> Reconfigured {
        let mut old = std::mem::take(&mut self.peers);
        let mut out = Reconfigured::default();
        for p in &config.peers {
            let kernel = kernel_incarnations
                .iter()
                .find(|(s, _)| *s == p.slot)
                .map_or([0; INCARNATION_LEN], |(_, i)| *i);
            let previous = old
                .iter()
                .position(|o| o.slot == p.slot)
                .map(|at| old.swap_remove(at));
            match previous {
                // The same kernel peer, and the controller's own view still matches it.
                Some(mut o)
                    if !is_zero(&kernel)
                        && o.kernel_incarnation == kernel
                        && o.public == p.public_key
                        && o.role == p.role
                        && o.path_budget == p.path_mtu =>
                {
                    o.name.clone_from(&p.name);
                    self.peers.push(o);
                    out.kept.push(p.slot);
                }
                other => {
                    if other.is_some() {
                        out.replaced.push(p.slot);
                    } else {
                        out.added.push(p.slot);
                    }
                    self.peers.push(PeerCtl {
                        name: p.name.clone(),
                        slot: p.slot,
                        public: p.public_key,
                        role: p.role,
                        path_budget: p.path_mtu,
                        kernel_incarnation: kernel,
                        state: State::Idle { not_before: now },
                        failures: 0,
                        cached: None,
                        rekey_requested: false,
                        session_hash: None,
                    });
                }
            }
        }
        out.removed = old.iter().map(|o| o.slot).collect();
        out.removed.sort_unstable();
        self.peers.sort_by_key(|p| p.slot);
        out
    }

    /// Begin an attempt. With `granted` (the session is established) the re-key keeps the flows: the
    /// kernel is asked for a continuity grant first and the mode 1 INIT follows its REKEY_GRANTED.
    /// Without it, or when no grant can be asked for, the attempt is fresh (mode 0).
    fn start_attempt<P: Port>(&mut self, i: usize, port: &mut P, now: Instant, granted: bool) {
        self.peers[i].rekey_requested = false;
        let (attempt_id, initiator_locator) = match self.begin_attempt(i, port, None) {
            Ok(x) => x,
            Err(e) => {
                self.fail_start(i, now, format!("cannot begin an attempt: {e}"));
                return;
            }
        };
        if granted && let Some(parent) = self.peers[i].session_hash {
            let slot = self.peers[i].slot;
            match port.rekey_request(slot, &self.peers[i].kernel_incarnation, &attempt_id) {
                Ok(()) => {
                    self.peers[i].state = State::AwaitGrant(Box::new(Awaiting {
                        attempt_id,
                        initiator_locator,
                        parent,
                        started: now,
                        next_repeat: Some(now + GRANT_REPEAT[0]),
                        repeats: 0,
                    }));
                    return;
                }
                Err(e) => {
                    // The kernel cannot grant (the session is closed or limits are hard): fresh.
                    let name = self.peers[i].name.clone();
                    self.say(format!(
                        "cannot ask {name} for a continuity grant ({e}); making a fresh attempt"
                    ));
                }
            }
        }
        self.send_init(
            i,
            port,
            now,
            now,
            Mode::Fresh,
            attempt_id,
            initiator_locator,
            [0; HASH_LEN],
        );
    }

    #[allow(clippy::too_many_arguments)]
    fn send_init<P: Port>(
        &mut self,
        i: usize,
        port: &mut P,
        now: Instant,
        began: Instant,
        mode: Mode,
        attempt_id: [u8; ATTEMPT_ID_LEN],
        initiator_locator: u64,
        parent: [u8; HASH_LEN],
    ) {
        let payload = HandshakePayload {
            mode,
            attempt_id,
            incarnation: self.incarnation,
            initiator_locator,
            responder_locator: 0,
            parent,
            path_budget: self.peers[i].path_budget,
        };
        let initiator = match Initiator::start(&self.key, &self.peers[i].public, payload) {
            Ok(x) => x,
            Err(e) => {
                self.fail_start(i, now, format!("cannot build INIT: {e}"));
                return;
            }
        };
        let slot = self.peers[i].slot;
        if let Err(e) = port.exchange_tx(slot, initiator.init_bytes()) {
            self.fail_start(i, now, format!("cannot send INIT: {e}"));
            return;
        }
        self.counts.attempts_started += 1;
        let name = self.peers[i].name.clone();
        let kind = if mode == Mode::Rekey {
            "granted re-key INIT"
        } else {
            "INIT"
        };
        self.say(format!("sent {kind} to {name}"));
        self.peers[i].state = State::Pending(Box::new(Pending {
            initiator,
            started: began,
            next_retry: now + RETRANSMIT[0],
            retries: 0,
        }));
    }

    /// REKEY_GRANTED did not arrive yet: ask again for the same attempt (a lost event is not re-raised
    /// by the kernel). A refused repeat is only logged; the wait then ends in the fresh fallback.
    fn repeat_grant_request<P: Port>(&mut self, i: usize, port: &mut P, now: Instant) {
        let slot = self.peers[i].slot;
        let inc = self.peers[i].kernel_incarnation;
        let State::AwaitGrant(a) = &mut self.peers[i].state else {
            return;
        };
        a.repeats += 1;
        a.next_repeat = GRANT_REPEAT.get(a.repeats).map(|d| now + *d);
        let attempt = a.attempt_id;
        if let Err(e) = port.rekey_request(slot, &inc, &attempt) {
            let name = self.peers[i].name.clone();
            self.say(format!("repeated grant request to {name} failed: {e}"));
        }
    }

    fn fail_start(&mut self, i: usize, now: Instant, why: String) {
        self.counts.attempts_failed += 1;
        let f = self.peers[i].failures;
        self.peers[i].failures += 1;
        self.say(why);
        self.peers[i].state = State::Idle {
            not_before: now + BACKOFF[f.min(BACKOFF.len() - 1)],
        };
    }

    fn retransmit<P: Port>(&mut self, i: usize, port: &mut P, now: Instant) {
        let slot = self.peers[i].slot;
        let State::Pending(p) = &mut self.peers[i].state else {
            return;
        };
        // The exact same INIT bytes are resent (TECHNICAL_DESIGN.md 5.3).
        let result = port.exchange_tx(slot, p.initiator.init_bytes());
        p.retries += 1;
        let step = RETRANSMIT[p.retries.min(RETRANSMIT.len() - 1)];
        p.next_retry = now + step;
        if let Err(e) = result {
            let msg = format!("INIT retransmit failed: {e}");
            self.say(msg);
        }
    }

    /// An INIT or RESPONSE the kernel forwarded. The kernel verified nothing but the
    /// outer source; everything cryptographic is checked here.
    pub fn on_exchange<P: Port>(&mut self, port: &mut P, ev: ExchangeRx, now: Instant) {
        let Some(i) = self.peers.iter().position(|p| p.slot == ev.slot) else {
            self.counts.rejected += 1;
            return;
        };
        self.peers[i].kernel_incarnation = ev.kernel_incarnation;
        match ev.bytes.get(4).copied() {
            Some(1) => self.on_init(i, port, &ev.bytes, now),
            Some(2) => self.on_response(i, port, &ev.bytes, now),
            _ => self.counts.rejected += 1,
        }
    }

    fn stage_keys<P: Port>(
        &mut self,
        i: usize,
        port: &mut P,
        keys: &SessionKeys,
    ) -> Result<(), PortError> {
        let slot = self.peers[i].slot;
        let mut r = port.stage(slot, &self.peers[i].kernel_incarnation, keys);
        // The kernel replaced the peer (a disruptive change): learn the new identity once.
        if matches!(&r, Err(e) if e.errno == Some(116)) {
            let inc = port.kernel_incarnation(slot)?;
            self.peers[i].kernel_incarnation = inc;
            r = port.stage(slot, &inc, keys);
        }
        r
    }

    fn on_init<P: Port>(&mut self, i: usize, port: &mut P, init: &[u8], now: Instant) {
        let name = self.peers[i].name.clone();
        if self.peers[i].role != Role::Responder {
            self.counts.rejected += 1;
            self.say(format!("ignored INIT from {name}: we are its initiator"));
            return;
        }
        let slot = self.peers[i].slot;
        // A byte-identical duplicate gets the identical cached RESPONSE, nothing else.
        if let Some(c) = &self.peers[i].cached
            && now.duration_since(c.at) < CACHE_LIFETIME
            && let Some(resp) = c.cache.replay_for(init)
        {
            let resp = resp.to_vec();
            self.counts.duplicates_answered += 1;
            if let Err(e) = port.exchange_tx(slot, &resp) {
                self.say(format!("cannot resend RESPONSE: {e}"));
            }
            return;
        }
        // The kernel issues the responder's locator, for the attempt of the INIT it delivered.
        let attempt = match parse_handshake(init) {
            Ok(d) => d.attempt_id,
            Err(e) => {
                self.counts.rejected += 1;
                self.say(format!("rejected INIT from {name}: {e}"));
                return;
            }
        };
        let responder_locator = match self.begin_attempt(i, port, Some(&attempt)) {
            Ok((_, locator)) => locator,
            Err(e) => {
                self.counts.rejected += 1;
                self.say(format!("cannot begin an attempt for {name}'s INIT: {e}"));
                return;
            }
        };
        let params = ResponderParams {
            incarnation: self.incarnation,
            responder_locator,
            path_budget: self.peers[i].path_budget,
            continuity_grant: self.peers[i].session_hash,
        };
        let public = self.peers[i].public;
        let responded = match respond(&self.key, &public, init, &params) {
            Ok(r) => r,
            Err(e) => {
                self.counts.rejected += 1;
                self.say(format!("rejected INIT from {name}: {e}"));
                return;
            }
        };
        // Install the keys before the RESPONSE leaves, so the initiator's first record
        // can already be opened; a failure to stage abandons the attempt.
        if let Err(e) = self.stage_keys(i, port, &responded.keys) {
            if e.errno == Some(EPERM) && responded.keys.mode == Mode::Rekey {
                self.say(format!(
                    "cannot answer {name}'s granted re-key INIT: the kernel refused its parent \
                     session ({e}); no RESPONSE is sent, the initiator's attempt will time out \
                     and fall back to a fresh one"
                ));
            } else {
                self.say(format!("cannot install keys for {name}: {e}"));
            }
            return;
        }
        if let Err(e) = port.exchange_tx(slot, &responded.response) {
            self.say(format!("cannot send RESPONSE to {name}: {e}"));
        }
        self.peers[i].cached = Some(Cached {
            cache: ResponseCache::new(init, &responded.response),
            at: now,
        });
        self.peers[i].state = State::Established { since: now };
        self.peers[i].session_hash = Some(responded.keys.handshake_hash);
        self.peers[i].failures = 0;
        self.counts.established += 1;
        self.say(format!("tunnel to {name} established (responder)"));
    }

    fn on_response<P: Port>(&mut self, i: usize, port: &mut P, response: &[u8], now: Instant) {
        let name = self.peers[i].name.clone();
        let State::Pending(p) = &mut self.peers[i].state else {
            // Not waiting for a RESPONSE (late duplicate, or never asked): drop it.
            self.counts.rejected += 1;
            return;
        };
        // A forged or stale RESPONSE leaves the attempt pending (Initiator::finish).
        let keys = match p.initiator.finish(response) {
            Ok(k) => k,
            Err(e) => {
                self.counts.rejected += 1;
                let note = match e {
                    ExchangeError::Completed => "late".to_owned(),
                    other => other.to_string(),
                };
                self.say(format!("rejected RESPONSE from {name}: {note}"));
                return;
            }
        };
        match self.stage_keys(i, port, &keys) {
            Ok(()) => {
                self.peers[i].state = State::Established { since: now };
                self.peers[i].session_hash = Some(keys.handshake_hash);
                self.peers[i].failures = 0;
                self.counts.established += 1;
                self.say(format!("tunnel to {name} established (initiator)"));
            }
            Err(e) if e.errno == Some(EPERM) && keys.mode == Mode::Rekey => {
                // The kernel refused the parent (stale or unknown) or the grant. Forget the session
                // hash and try again at once as a fresh attempt: it cannot be refused the same way,
                // and a failure of that attempt follows the normal backoff.
                self.counts.attempts_failed += 1;
                self.peers[i].session_hash = None;
                self.say(format!(
                    "the kernel refused the parent session of the granted re-key with {name} ({e}); \
                     forgetting it and making a fresh attempt"
                ));
                self.start_attempt(i, port, now, false);
            }
            Err(e) => {
                self.counts.attempts_failed += 1;
                self.say(format!("cannot install keys for {name}: {e}"));
                self.peers[i].state = State::Idle {
                    not_before: now + BACKOFF[0],
                };
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::PrivateKey;
    use std::sync::{Arc, Mutex};

    const A_KEY: &str = "A";
    const B_KEY: &str = "B";

    #[derive(Default)]
    struct Wire {
        /// Messages sent toward the other side, by the sender's name.
        to_b: Vec<Vec<u8>>,
        to_a: Vec<Vec<u8>>,
        drop_next_to_b: usize,
        drop_next_to_a: usize,
        /// REKEY_GRANTED events owed to the initiator, and whether grants are withheld.
        grants_to_a: Vec<(u8, [u8; 16])>,
        withhold_grants: bool,
        /// The grants of the next this-many REKEY_REQUESTs are lost (the event is raised once).
        lose_grants: usize,
        rekey_requests: usize,
    }

    /// (slot, tx key, rx key, local locator, remote locator) of one installed generation.
    type Staged = (u8, [u8; 32], [u8; 32], u64, u64);

    struct TestPort {
        who: &'static str,
        wire: Arc<Mutex<Wire>>,
        staged: Vec<Staged>,
        stale_once: bool,
        refreshed: Vec<u8>,
        /// The mode and parent of each installed generation.
        modes: Vec<(Mode, [u8; HASH_LEN])>,
        /// The handshake hash of each installed generation.
        hashes: Vec<[u8; HASH_LEN]>,
        /// Refuse the next this-many mode 1 installations with -EPERM, as the kernel does for a
        /// stale parent or a missing grant.
        refuse_rekey: usize,
        begins: u64,
    }

    impl TestPort {
        fn new(who: &'static str, wire: &Arc<Mutex<Wire>>) -> Self {
            Self {
                who,
                wire: wire.clone(),
                staged: Vec::new(),
                stale_once: false,
                refreshed: Vec::new(),
                modes: Vec::new(),
                hashes: Vec::new(),
                refuse_rekey: 0,
                begins: 0,
            }
        }
    }

    impl Port for TestPort {
        fn begin_attempt(
            &mut self,
            _slot: u8,
            _inc: &[u8; INCARNATION_LEN],
            given: Option<&[u8; ATTEMPT_ID_LEN]>,
        ) -> Result<([u8; ATTEMPT_ID_LEN], u64), PortError> {
            // Stands in for the kernel: a fresh ID (unless the INIT's is given) and a nonzero locator.
            self.begins += 1;
            let id = given
                .copied()
                .unwrap_or([self.begins as u8; ATTEMPT_ID_LEN]);
            let tag = if self.who == A_KEY { 0xa000 } else { 0xb000 };
            Ok((id, tag + self.begins))
        }

        fn rekey_request(
            &mut self,
            slot: u8,
            _inc: &[u8; INCARNATION_LEN],
            attempt: &[u8; ATTEMPT_ID_LEN],
        ) -> Result<(), PortError> {
            // Stands in for the kernel asking the responder: the grant comes back as an event.
            let mut w = self.wire.lock().unwrap();
            w.rekey_requests += 1;
            if w.lose_grants > 0 {
                w.lose_grants -= 1;
            } else if !w.withhold_grants {
                w.grants_to_a.push((slot, *attempt));
            }
            Ok(())
        }

        fn exchange_tx(&mut self, _slot: u8, bytes: &[u8]) -> Result<(), PortError> {
            let mut w = self.wire.lock().unwrap();
            if self.who == A_KEY {
                if w.drop_next_to_b > 0 {
                    w.drop_next_to_b -= 1;
                } else {
                    w.to_b.push(bytes.to_vec());
                }
            } else if w.drop_next_to_a > 0 {
                w.drop_next_to_a -= 1;
            } else {
                w.to_a.push(bytes.to_vec());
            }
            Ok(())
        }

        fn stage(
            &mut self,
            slot: u8,
            _inc: &[u8; INCARNATION_LEN],
            keys: &SessionKeys,
        ) -> Result<(), PortError> {
            if self.stale_once {
                self.stale_once = false;
                return Err(PortError {
                    what: "stale".into(),
                    errno: Some(116),
                });
            }
            if keys.mode == Mode::Rekey && self.refuse_rekey > 0 {
                self.refuse_rekey -= 1;
                return Err(PortError {
                    what: "not permitted".into(),
                    errno: Some(1),
                });
            }
            self.hashes.push(keys.handshake_hash);
            self.modes.push((keys.mode, keys.parent));
            self.staged.push((
                slot,
                *keys.tx,
                *keys.rx,
                keys.local_locator,
                keys.remote_locator,
            ));
            Ok(())
        }

        fn kernel_incarnation(&mut self, slot: u8) -> Result<[u8; INCARNATION_LEN], PortError> {
            self.refreshed.push(slot);
            Ok([0x77; INCARNATION_LEN])
        }
    }

    fn config(
        role_a: &str,
        role_b: &str,
        a_pub: &PublicKey,
        b_pub: &PublicKey,
    ) -> (GatewayConfig, GatewayConfig) {
        let a = format!(
            r#"
[gateway]
name = "a"
private_key_file = "/tmp/a.key"
lan_interface = "lan0"
lan_address = "10.77.1.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.11/24"
[[peer]]
name = "b"
slot = 1
public_key = "{b_pub}"
endpoint = "172.31.250.12"
role = "{role_a}"
path_mtu = 1500
prefixes = ["10.77.2.0/24"]
[[rule]]
id = 1
peer = "b"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8080"
"#
        );
        let b = format!(
            r#"
[gateway]
name = "b"
private_key_file = "/tmp/b.key"
lan_interface = "lan0"
lan_address = "10.77.2.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.12/24"
[[peer]]
name = "a"
slot = 1
public_key = "{a_pub}"
endpoint = "172.31.250.11"
role = "{role_b}"
path_mtu = 1500
prefixes = ["10.77.1.0/24"]
[[rule]]
id = 1
peer = "a"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8080"
"#
        );
        (
            GatewayConfig::parse(&a).unwrap(),
            GatewayConfig::parse(&b).unwrap(),
        )
    }

    struct Pair {
        a: Controller,
        b: Controller,
        pa: TestPort,
        pb: TestPort,
        wire: Arc<Mutex<Wire>>,
        logs: Arc<Mutex<Vec<String>>>,
        now: Instant,
    }

    fn rng(seed: u8) -> Entropy {
        let mut n = seed;
        Box::new(move |buf: &mut [u8]| {
            for b in buf.iter_mut() {
                n = n.wrapping_mul(31).wrapping_add(17);
                *b = n | 1;
            }
        })
    }

    fn pair() -> Pair {
        let ka = PrivateKey::generate().unwrap();
        let kb = PrivateKey::generate().unwrap();
        let (cfg_a, cfg_b) = config("initiator", "responder", &ka.public_key(), &kb.public_key());
        let wire = Arc::new(Mutex::new(Wire::default()));
        let logs = Arc::new(Mutex::new(Vec::new()));
        let log = |tag: &'static str, logs: &Arc<Mutex<Vec<String>>>| -> Log {
            let logs = logs.clone();
            Box::new(move |m| logs.lock().unwrap().push(format!("{tag}: {m}")))
        };
        let now = Instant::now();
        let a = Controller::new(ka, &cfg_a, &[(1, [0xA1; 16])], rng(3), log("a", &logs), now);
        let b = Controller::new(kb, &cfg_b, &[(1, [0xB1; 16])], rng(9), log("b", &logs), now);
        Pair {
            a,
            b,
            pa: TestPort::new(A_KEY, &wire),
            pb: TestPort::new(B_KEY, &wire),
            wire,
            logs,
            now,
        }
    }

    impl Pair {
        /// Deliver everything in flight, in order, until nothing moves.
        fn pump(&mut self) {
            loop {
                let (to_a, to_b, grants) = {
                    let mut w = self.wire.lock().unwrap();
                    (
                        std::mem::take(&mut w.to_a),
                        std::mem::take(&mut w.to_b),
                        std::mem::take(&mut w.grants_to_a),
                    )
                };
                if to_a.is_empty() && to_b.is_empty() && grants.is_empty() {
                    return;
                }
                for (slot, attempt) in grants {
                    self.a
                        .on_rekey_granted(&mut self.pa, slot, attempt, self.now);
                }
                for m in to_b {
                    let ev = ExchangeRx {
                        slot: 1,
                        kernel_incarnation: [0xB1; 16],
                        bytes: m,
                    };
                    self.b.on_exchange(&mut self.pb, ev, self.now);
                }
                for m in to_a {
                    let ev = ExchangeRx {
                        slot: 1,
                        kernel_incarnation: [0xA1; 16],
                        bytes: m,
                    };
                    self.a.on_exchange(&mut self.pa, ev, self.now);
                }
            }
        }

        fn advance(&mut self, d: Duration) {
            self.now += d;
            self.a.tick(&mut self.pa, self.now);
            self.b.tick(&mut self.pb, self.now);
            self.pump();
        }
    }

    #[test]
    fn both_sides_install_mirrored_keys_and_locators() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        assert!(p.a.is_established(1) && p.b.is_established(1));
        assert_eq!(p.pa.staged.len(), 1);
        assert_eq!(p.pb.staged.len(), 1);
        let (_, a_tx, a_rx, a_loc_l, a_loc_r) = p.pa.staged[0];
        let (_, b_tx, b_rx, b_loc_l, b_loc_r) = p.pb.staged[0];
        // Initiator TX = k1 = responder RX, and the other way round.
        assert_eq!(a_tx, b_rx);
        assert_eq!(a_rx, b_tx);
        assert_ne!(a_tx, a_rx, "the two directions must use different keys");
        assert_eq!((a_loc_l, a_loc_r), (b_loc_r, b_loc_l));
        assert!(a_loc_l != 0 && a_loc_r != 0);
    }

    #[test]
    fn a_lost_init_is_retransmitted_with_the_same_bytes() {
        let mut p = pair();
        p.wire.lock().unwrap().drop_next_to_b = 1;
        p.advance(Duration::ZERO); // INIT sent and lost
        assert!(!p.a.is_established(1));
        p.advance(Duration::from_millis(499));
        assert!(p.pa.staged.is_empty(), "no retransmit before 0.5 s");
        p.advance(Duration::from_millis(2));
        assert!(p.a.is_established(1) && p.b.is_established(1));
        assert_eq!(p.pb.staged.len(), 1);
    }

    #[test]
    fn a_duplicate_init_gets_the_cached_response_and_keys_are_not_installed_twice() {
        let mut p = pair();
        p.wire.lock().unwrap().drop_next_to_a = 1; // the first RESPONSE is lost
        p.advance(Duration::ZERO);
        assert!(p.b.is_established(1) && !p.a.is_established(1));
        p.advance(Duration::from_millis(600)); // INIT retransmitted, cached RESPONSE returned
        assert!(p.a.is_established(1));
        assert_eq!(
            p.pb.staged.len(),
            1,
            "the responder must not install a second generation"
        );
        assert_eq!(p.b.counts().duplicates_answered, 1);
        assert_eq!(p.pa.staged[0].1, p.pb.staged[0].2);
    }

    #[test]
    fn forged_and_misdirected_messages_change_nothing() {
        let mut p = pair();
        p.wire.lock().unwrap().drop_next_to_b = 1;
        p.advance(Duration::ZERO); // a pending attempt exists, B has seen nothing
        // A forged RESPONSE with a valid envelope shape but garbage Noise bytes.
        let mut forged = vec![0u8; 176];
        forged[0..4].copy_from_slice(b"VPT1");
        forged[4] = 2;
        forged[6..8].copy_from_slice(&160u16.to_be_bytes());
        forged[8] = 1;
        let ev = ExchangeRx {
            slot: 1,
            kernel_incarnation: [0xA1; 16],
            bytes: forged,
        };
        p.a.on_exchange(&mut p.pa, ev, p.now);
        assert!(!p.a.is_established(1));
        assert_eq!(p.a.counts().rejected, 1);
        // The attempt survived the forgery: the retransmitted INIT still completes it.
        p.advance(Duration::from_millis(600));
        assert!(p.a.is_established(1));
        // Garbage INIT at the responder, and an INIT for an unknown slot, are rejected.
        let junk = ExchangeRx {
            slot: 1,
            kernel_incarnation: [0xB1; 16],
            bytes: vec![1; 224],
        };
        let before = p.b.counts().rejected;
        p.b.on_exchange(&mut p.pb, junk, p.now);
        assert_eq!(p.b.counts().rejected, before + 1);
        let unknown = ExchangeRx {
            slot: 3,
            kernel_incarnation: [0; 16],
            bytes: vec![0; 224],
        };
        p.b.on_exchange(&mut p.pb, unknown, p.now);
        assert_eq!(p.b.counts().rejected, before + 2);
    }

    #[test]
    fn an_attempt_that_never_completes_backs_off_and_retries() {
        let mut p = pair();
        p.wire.lock().unwrap().drop_next_to_b = 1000;
        p.advance(Duration::ZERO);
        // 10 s deadline passes with no answer.
        for _ in 0..12 {
            p.advance(Duration::from_secs(1));
        }
        assert_eq!(p.a.counts().attempts_failed, 1);
        assert!(!p.a.is_established(1));
        p.wire.lock().unwrap().drop_next_to_b = 0;
        // Backoff after the first failure is 1 s; the next attempt then succeeds.
        p.advance(Duration::from_secs(2));
        assert!(p.a.is_established(1), "logs: {:?}", p.logs.lock().unwrap());
        assert_eq!(p.a.counts().attempts_started, 2);
    }

    #[test]
    fn the_initiator_re_establishes_before_the_kernel_key_age_limit() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        let first_tx = p.pa.staged[0].1;
        p.advance(REKEY_AFTER - Duration::from_secs(1));
        assert_eq!(
            p.pa.staged.len(),
            1,
            "no new exchange before the rekey time"
        );
        p.advance(Duration::from_secs(2));
        assert_eq!(p.pa.staged.len(), 2);
        assert_eq!(p.pb.staged.len(), 2);
        assert_ne!(p.pa.staged[1].1, first_tx, "fresh keys, not the old ones");
        assert_eq!(p.pa.staged[1].1, p.pb.staged[1].2);
        assert!(p.a.is_established(1) && p.b.is_established(1));
    }

    #[test]
    fn a_rekey_asks_for_a_grant_and_the_granted_init_names_the_old_session() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        assert_eq!(p.pa.modes, vec![(Mode::Fresh, [0; HASH_LEN])]);
        assert_eq!(p.wire.lock().unwrap().rekey_requests, 0);
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        assert_eq!(
            p.wire.lock().unwrap().rekey_requests,
            1,
            "the grant was asked for first"
        );
        assert_eq!(p.pa.staged.len(), 2);
        assert_eq!(p.pb.staged.len(), 2);
        let (mode_a, parent_a) = p.pa.modes[1];
        let (mode_b, parent_b) = p.pb.modes[1];
        assert_eq!((mode_a, mode_b), (Mode::Rekey, Mode::Rekey));
        assert_ne!(parent_a, [0; HASH_LEN]);
        assert_eq!(
            parent_a, parent_b,
            "both sides name the same parent session"
        );
        assert!(p.a.is_established(1) && p.b.is_established(1));
        // The next rekey continues the new session, not the first.
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        assert_eq!(p.pa.modes.len(), 3);
        assert_ne!(p.pa.modes[2].1, parent_a);
        // The parents are the real handshake hashes of the sessions being continued, not just some
        // value both sides happen to agree on.
        assert_eq!(parent_a, p.pa.hashes[0]);
        assert_eq!(parent_b, p.pb.hashes[0]);
        assert_eq!(p.pa.modes[2].1, p.pa.hashes[1]);
        assert_eq!(p.pb.modes[2].1, p.pb.hashes[1]);
    }

    #[test]
    fn without_a_grant_the_initiator_falls_back_to_a_fresh_attempt() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        p.wire.lock().unwrap().withhold_grants = true;
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        assert_eq!(p.pa.staged.len(), 1, "no INIT while waiting for the grant");
        assert!(p.a.is_established(1), "the old session is still in use");
        // The wait is exactly five seconds: nothing happens at 4.999 s, the fallback at 5 s.
        for _ in 0..4 {
            p.advance(Duration::from_secs(1));
        }
        p.advance(Duration::from_millis(999));
        assert_eq!(p.pa.staged.len(), 1);
        assert!(p.a.is_established(1));
        p.advance(Duration::from_millis(1));
        assert_eq!(p.pa.staged.len(), 2);
        assert_eq!(p.pa.modes[1], (Mode::Fresh, [0; HASH_LEN]));
        assert_eq!(p.pb.modes[1], (Mode::Fresh, [0; HASH_LEN]));
        // The request was repeated twice for the same attempt (1 s, then 2 s later), no more.
        assert_eq!(p.wire.lock().unwrap().rekey_requests, 3);
        assert!(
            p.logs
                .lock()
                .unwrap()
                .iter()
                .any(|l| l.contains("no continuity grant"))
        );
    }

    #[test]
    fn a_lost_grant_event_is_recovered_by_repeating_the_request() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        p.wire.lock().unwrap().lose_grants = 1;
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        assert_eq!(p.pa.staged.len(), 1, "the first grant event was lost");
        p.advance(Duration::from_millis(999));
        assert_eq!(p.wire.lock().unwrap().rekey_requests, 1, "not before 1 s");
        p.advance(Duration::from_millis(1));
        assert_eq!(p.wire.lock().unwrap().rekey_requests, 2);
        assert_eq!(p.pa.modes[1].0, Mode::Rekey, "granted, not a fresh attempt");
        assert_eq!(p.pb.modes[1].0, Mode::Rekey);
        assert!(
            !p.logs
                .lock()
                .unwrap()
                .iter()
                .any(|l| l.contains("fresh attempt"))
        );
    }

    #[test]
    fn the_exchange_deadline_runs_from_the_attempt_not_from_the_late_grant() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        {
            let mut w = p.wire.lock().unwrap();
            w.lose_grants = 2; // The grant only arrives with the repeat at 3 s.
            w.drop_next_to_a = usize::MAX; // No RESPONSE ever reaches the initiator.
        }
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        p.advance(Duration::from_secs(1));
        p.advance(Duration::from_secs(2));
        assert_eq!(p.wire.lock().unwrap().rekey_requests, 3);
        assert_eq!(p.a.counts().attempts_started, 2, "the granted INIT is out");
        // The kernel's window began at BEGIN_ATTEMPT, 3 s ago: 7 s are left, not 10.
        p.advance(Duration::from_millis(6999));
        assert_eq!(p.a.counts().attempts_failed, 0);
        p.advance(Duration::from_millis(1));
        assert_eq!(p.a.counts().attempts_failed, 1);
    }

    #[test]
    fn a_refused_parent_makes_the_initiator_fall_back_to_a_fresh_attempt_at_once() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        let first_hash = p.pa.hashes[0];
        p.pa.refuse_rekey = 1;
        // Step by hand so the state right after the refusal can be looked at.
        p.now += REKEY_AFTER + Duration::from_secs(1);
        p.a.tick(&mut p.pa, p.now);
        let grants = std::mem::take(&mut p.wire.lock().unwrap().grants_to_a);
        for (slot, attempt) in grants {
            p.a.on_rekey_granted(&mut p.pa, slot, attempt, p.now);
        }
        let to_b = std::mem::take(&mut p.wire.lock().unwrap().to_b);
        assert_eq!(to_b.len(), 1);
        for m in to_b {
            let ev = ExchangeRx {
                slot: 1,
                kernel_incarnation: [0xB1; 16],
                bytes: m,
            };
            p.b.on_exchange(&mut p.pb, ev, p.now);
        }
        let to_a = std::mem::take(&mut p.wire.lock().unwrap().to_a);
        for m in to_a {
            let ev = ExchangeRx {
                slot: 1,
                kernel_incarnation: [0xA1; 16],
                bytes: m,
            };
            p.a.on_exchange(&mut p.pa, ev, p.now);
        }
        // Immediately (no time passed): the stored hash is forgotten and a mode 0 INIT is out.
        assert_eq!(p.a.peers[0].session_hash, None);
        assert!(matches!(p.a.peers[0].state, State::Pending(_)));
        assert_eq!(p.wire.lock().unwrap().to_b.len(), 1);
        assert_eq!(p.a.counts().attempts_failed, 1);
        assert_eq!(p.wire.lock().unwrap().rekey_requests, 1, "no second grant");
        assert!(
            p.logs
                .lock()
                .unwrap()
                .iter()
                .any(|l| l.contains("kernel refused the parent session"))
        );
        // The fresh attempt completes and the session continues with new keys.
        p.pump();
        assert!(p.a.is_established(1) && p.b.is_established(1));
        assert_eq!(p.pa.modes, vec![(Mode::Fresh, [0; HASH_LEN]); 2]);
        assert_ne!(p.pa.hashes[1], first_hash);
        assert_eq!(p.a.peers[0].session_hash, Some(p.pa.hashes[1]));
    }

    #[test]
    fn a_responder_that_the_kernel_refuses_logs_it_and_the_initiator_times_out_to_fresh() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        p.pb.refuse_rekey = usize::MAX;
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        assert_eq!(p.pb.staged.len(), 1, "the responder could not install");
        assert!(!p.a.is_established(1), "the initiator is still waiting");
        assert!(
            p.logs
                .lock()
                .unwrap()
                .iter()
                .any(|l| l.contains("b: cannot answer") && l.contains("refused its parent"))
        );
        // The attempt times out (10 s), backs off 1 s, and the next attempt is fresh.
        p.advance(ATTEMPT_DEADLINE);
        assert_eq!(p.a.counts().attempts_failed, 1);
        p.advance(Duration::from_secs(1));
        assert_eq!(p.pa.modes[1], (Mode::Fresh, [0; HASH_LEN]));
        assert_eq!(p.pb.modes[1], (Mode::Fresh, [0; HASH_LEN]));
        assert!(p.a.is_established(1) && p.b.is_established(1));
    }

    #[test]
    fn a_stale_or_foreign_grant_does_nothing() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        p.wire.lock().unwrap().withhold_grants = true;
        p.advance(REKEY_AFTER + Duration::from_secs(1));
        // A grant for another attempt is ignored and the initiator keeps waiting.
        p.a.on_rekey_granted(&mut p.pa, 1, [0x99; 16], p.now);
        p.pump();
        assert_eq!(p.pa.staged.len(), 1);
        assert!(matches!(p.a.peers[0].state, State::AwaitGrant(_)));
        assert_eq!(p.a.counts().attempts_started, 1);
        // A grant for an unknown slot changes nothing either.
        p.a.on_rekey_granted(&mut p.pa, 9, [1; 16], p.now);
        assert!(matches!(p.a.peers[0].state, State::AwaitGrant(_)));
        assert_eq!(p.a.counts().attempts_started, 1);
        assert!(!p.a.on_rekey_needed(9), "an unknown slot is not re-keyed");
        assert!(!p.b.on_rekey_needed(1), "only the initiator re-keys");
    }

    #[test]
    fn a_grant_with_nothing_waiting_does_not_start_another_exchange() {
        // Established: the attempt ID is irrelevant, nothing is waiting.
        let mut p = pair();
        p.advance(Duration::ZERO);
        let attempt = [p.pa.begins as u8; 16];
        p.a.on_rekey_granted(&mut p.pa, 1, attempt, p.now);
        assert_eq!(p.a.counts().attempts_started, 1);
        assert!(matches!(p.a.peers[0].state, State::Established { .. }));
        assert!(p.wire.lock().unwrap().to_b.is_empty());

        // Pending (a fresh INIT is out), with the very attempt ID of that INIT.
        let mut q = pair();
        q.wire.lock().unwrap().drop_next_to_b = usize::MAX;
        q.advance(Duration::ZERO);
        assert!(matches!(q.a.peers[0].state, State::Pending(_)));
        let attempt = [q.pa.begins as u8; 16];
        q.a.on_rekey_granted(&mut q.pa, 1, attempt, q.now);
        assert_eq!(q.a.counts().attempts_started, 1, "no second INIT");
        assert!(matches!(q.a.peers[0].state, State::Pending(_)));
    }

    #[test]
    fn a_rekey_request_is_taken_only_from_an_established_initiator() {
        let mut p = pair();
        assert!(!p.a.on_rekey_needed(1), "idle, nothing to re-key yet");
        p.wire.lock().unwrap().drop_next_to_b = usize::MAX;
        p.advance(Duration::ZERO);
        assert!(!p.a.on_rekey_needed(1), "pending");
        p.wire.lock().unwrap().drop_next_to_b = 0;
        p.advance(Duration::from_secs(1));
        assert!(p.a.is_established(1));
        p.wire.lock().unwrap().withhold_grants = true;
        assert!(p.a.on_rekey_needed(1), "an established initiator re-keys");
        p.advance(Duration::from_millis(1));
        assert!(matches!(p.a.peers[0].state, State::AwaitGrant(_)));
        assert!(!p.a.on_rekey_needed(1), "waiting for a grant");
    }

    #[test]
    fn session_hash_survives_a_reconfigure_for_a_kept_peer_and_is_cleared_for_a_replaced_one() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        let hash = p.a.peers[0].session_hash;
        assert_eq!(hash, Some(p.pa.hashes[0]));
        let key_b = p.a.peers[0].public;
        let cfg = gw_a(&[("b", 1, &key_b, "initiator", "10.77.2.0/24")], "9090");
        let r = p.a.reconfigure(&cfg, &[(1, [0xA1; 16])], p.now);
        assert_eq!(r.kept, vec![1]);
        assert_eq!(p.a.peers[0].session_hash, hash, "kept peer, kept session");
        let r = p.a.reconfigure(&cfg, &[(1, [0xA2; 16])], p.now);
        assert_eq!(r.replaced, vec![1]);
        assert_eq!(p.a.peers[0].session_hash, None, "a new peer has no session");
    }

    #[test]
    fn a_usage_request_re_establishes_early_and_only_the_initiator_can_start_it() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        assert_eq!(p.pa.staged.len(), 1);
        assert_eq!(p.b.request_rekey(), 0, "a responder never initiates");
        p.advance(Duration::from_secs(1));
        assert_eq!(p.pa.staged.len(), 1, "nothing happens without a request");
        let first_tx = p.pa.staged[0].1;
        assert_eq!(p.a.request_rekey(), 1);
        p.advance(Duration::from_secs(1));
        assert_eq!(
            p.pa.staged.len(),
            2,
            "a new exchange well before the age timer"
        );
        assert_ne!(p.pa.staged[1].1, first_tx);
        assert_eq!(p.pa.staged[1].1, p.pb.staged[1].2);
        p.advance(Duration::from_secs(5));
        assert_eq!(p.pa.staged.len(), 2, "one request starts one exchange");
    }

    #[test]
    fn a_stale_kernel_incarnation_is_refreshed_once_and_the_install_retried() {
        let mut p = pair();
        p.pb.stale_once = true;
        p.advance(Duration::ZERO);
        assert!(p.b.is_established(1));
        assert_eq!(p.pb.refreshed, vec![1]);
        assert_eq!(p.pb.staged.len(), 1);
    }

    #[test]
    fn an_initiator_ignores_an_init_and_a_responder_never_initiates() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        // B is the responder: it must have sent only a RESPONSE, never an INIT of its own.
        let logs = p.logs.lock().unwrap().clone();
        assert!(
            !logs.iter().any(|l| l.starts_with("b: sent INIT")),
            "{logs:?}"
        );
        let rejected = p.a.counts().rejected;
        let init = vec![0u8; 224];
        let mut bytes = init;
        bytes[4] = 1;
        p.a.on_exchange(
            &mut p.pa,
            ExchangeRx {
                slot: 1,
                kernel_incarnation: [0xA1; 16],
                bytes,
            },
            p.now,
        );
        assert_eq!(p.a.counts().rejected, rejected + 1);
    }

    // ---- Live reconfiguration ---------------------------------------------------------

    /// A's configuration with the given peers `(name, slot, key, role, prefix)` and one rule
    /// per peer on `port`.
    fn gw_a(peers: &[(&str, u8, &PublicKey, &str, &str)], port: &str) -> GatewayConfig {
        let mut text = String::from(
            r#"
[gateway]
name = "a"
private_key_file = "/tmp/a.key"
lan_interface = "lan0"
lan_address = "10.77.1.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.11/24"
"#,
        );
        for (i, (name, slot, key, role, prefix)) in peers.iter().enumerate() {
            let net = prefix.trim_end_matches(".0/24");
            text.push_str(&format!(
                r#"
[[peer]]
name = "{name}"
slot = {slot}
public_key = "{key}"
endpoint = "172.31.250.{}"
role = "{role}"
path_mtu = 1500
prefixes = ["{prefix}"]
[[rule]]
id = {}
peer = "{name}"
source = "10.77.1.10/32"
destination = "{net}.10/32"
protocol = "tcp"
ports = "{port}"
"#,
                12 + i,
                i + 1
            ));
        }
        GatewayConfig::parse(&text).unwrap()
    }

    const INC_A: [u8; 16] = [0xA1; 16];

    fn lone_initiator() -> (Controller, TestPort, PublicKey, Arc<Mutex<Wire>>, Instant) {
        let ka = PrivateKey::generate().unwrap();
        let kb = PrivateKey::generate().unwrap().public_key();
        let cfg = gw_a(&[("b", 1, &kb, "initiator", "10.77.2.0/24")], "8080");
        let wire = Arc::new(Mutex::new(Wire::default()));
        let now = Instant::now();
        let c = Controller::new(ka, &cfg, &[(1, INC_A)], rng(3), Box::new(|_| {}), now);
        (c, TestPort::new(A_KEY, &wire), kb, wire, now)
    }

    #[test]
    fn a_policy_only_reload_keeps_the_session_and_its_timers() {
        let mut p = pair();
        p.advance(Duration::ZERO);
        assert!(p.a.is_established(1) && p.b.is_established(1));
        let started = p.a.counts().attempts_started;

        // The kernel kept the peer (same incarnation): only the name and a rule changed.
        // Same peer key and role as the running pair, different rule port.
        let key_b = p.a.peers[0].public;
        let text = format!(
            r#"
[gateway]
name = "a"
private_key_file = "/tmp/a.key"
lan_interface = "lan0"
lan_address = "10.77.1.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.11/24"
[[peer]]
name = "b-renamed"
slot = 1
public_key = "{key_b}"
endpoint = "172.31.250.12"
role = "initiator"
path_mtu = 1500
prefixes = ["10.77.2.0/24"]
[[rule]]
id = 1
peer = "b-renamed"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "9090"
"#
        );
        let cfg = GatewayConfig::parse(&text).unwrap();
        let r = p.a.reconfigure(&cfg, &[(1, [0xA1; 16])], p.now);
        assert_eq!(r.kept, vec![1]);
        assert!(r.replaced.is_empty() && r.added.is_empty() && r.removed.is_empty());
        assert!(p.a.is_established(1), "the session must survive");
        assert_eq!(p.a.peers[0].name, "b-renamed");

        // No new exchange starts, and the original 90 s re-key timer still applies.
        p.advance(Duration::from_secs(60));
        assert_eq!(p.a.counts().attempts_started, started);
        p.advance(Duration::from_secs(31));
        assert_eq!(p.a.counts().attempts_started, started + 1);
    }

    #[test]
    fn a_replaced_peer_starts_a_fresh_exchange() {
        let (mut c, mut port, kb, wire, now) = lone_initiator();
        c.tick(&mut port, now);
        assert_eq!(c.counts().attempts_started, 1);
        wire.lock().unwrap().to_b.clear();

        // The kernel replaced the peer (a disruptive change): new incarnation.
        let cfg = gw_a(&[("b", 1, &kb, "initiator", "10.77.2.0/24")], "8080");
        let r = c.reconfigure(&cfg, &[(1, [0xA2; 16])], now);
        assert_eq!(r.replaced, vec![1]);
        assert!(r.kept.is_empty());
        assert!(!c.is_established(1));
        c.tick(&mut port, now);
        assert_eq!(c.counts().attempts_started, 2, "an exchange starts at once");
        assert_eq!(wire.lock().unwrap().to_b.len(), 1);
        assert_eq!(c.peers[0].kernel_incarnation, [0xA2; 16]);
    }

    #[test]
    fn a_missing_or_zero_kernel_incarnation_is_never_treated_as_kept() {
        let (mut c, mut port, kb, _wire, now) = lone_initiator();
        c.tick(&mut port, now);
        let cfg = gw_a(&[("b", 1, &kb, "initiator", "10.77.2.0/24")], "8080");
        assert_eq!(c.reconfigure(&cfg, &[], now).replaced, vec![1]);
        let r = c.reconfigure(&cfg, &[(1, [0; 16])], now);
        assert_eq!(r.replaced, vec![1]);
        assert!(r.kept.is_empty());
    }

    #[test]
    fn the_controllers_own_view_must_match_the_kernels_to_keep_a_peer() {
        // The same incarnation but a different role or key means the two have diverged:
        // start over rather than trust the old state.
        let (mut c, mut port, kb, _wire, now) = lone_initiator();
        c.tick(&mut port, now);
        let other = PrivateKey::generate().unwrap().public_key();
        let role_flip = gw_a(&[("b", 1, &kb, "responder", "10.77.2.0/24")], "8080");
        assert_eq!(
            c.reconfigure(&role_flip, &[(1, INC_A)], now).replaced,
            vec![1]
        );
        let key_flip = gw_a(&[("b", 1, &other, "responder", "10.77.2.0/24")], "8080");
        assert_eq!(
            c.reconfigure(&key_flip, &[(1, INC_A)], now).replaced,
            vec![1]
        );
    }

    #[test]
    fn removed_and_added_slots_are_followed() {
        let ka = PrivateKey::generate().unwrap();
        let kb = PrivateKey::generate().unwrap().public_key();
        let kc = PrivateKey::generate().unwrap().public_key();
        let two = gw_a(
            &[
                ("b", 1, &kb, "initiator", "10.77.2.0/24"),
                ("c", 2, &kc, "initiator", "10.77.3.0/24"),
            ],
            "8080",
        );
        let wire = Arc::new(Mutex::new(Wire::default()));
        let mut port = TestPort::new(A_KEY, &wire);
        let now = Instant::now();
        let mut c = Controller::new(
            ka,
            &two,
            &[(1, [1; 16]), (2, [2; 16])],
            rng(3),
            Box::new(|_| {}),
            now,
        );
        c.tick(&mut port, now);
        assert_eq!(wire.lock().unwrap().to_b.len(), 2);
        wire.lock().unwrap().to_b.clear();

        // Slot 2 goes away; slot 1 is untouched.
        let one = gw_a(&[("b", 1, &kb, "initiator", "10.77.2.0/24")], "8080");
        let r = c.reconfigure(&one, &[(1, [1; 16])], now);
        assert_eq!((r.kept.clone(), r.removed.clone()), (vec![1], vec![2]));
        assert_eq!(c.peers.len(), 1);
        c.tick(&mut port, now + Duration::from_millis(600));
        let sent = wire.lock().unwrap().to_b.len();
        assert_eq!(sent, 1, "only slot 1 retransmits; slot 2 is gone");

        // Slot 2 comes back, as a new peer with a new incarnation.
        let r = c.reconfigure(&two, &[(1, [1; 16]), (2, [9; 16])], now);
        assert_eq!((r.kept, r.added), (vec![1], vec![2]));
        assert_eq!(
            c.peers.iter().map(|p| p.slot).collect::<Vec<_>>(),
            vec![1, 2]
        );
    }

    #[test]
    fn a_replaced_responder_forgets_its_cached_response() {
        let mut p = pair();
        p.a.tick(&mut p.pa, p.now);
        let init = p.wire.lock().unwrap().to_b[0].clone();
        p.pump();
        assert!(p.b.is_established(1));
        // The original INIT, replayed byte for byte, is answered from the cache.
        p.b.on_exchange(
            &mut p.pb,
            ExchangeRx {
                slot: 1,
                kernel_incarnation: [0xB1; 16],
                bytes: init.clone(),
            },
            p.now,
        );
        assert_eq!(p.b.counts().duplicates_answered, 1);

        // The kernel replaces B's peer: the cached RESPONSE belongs to the old peer.
        let cfg = {
            let key_a = p.b.peers[0].public;
            let text = format!(
                r#"
[gateway]
name = "b"
private_key_file = "/tmp/b.key"
lan_interface = "lan0"
lan_address = "10.77.2.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.12/24"
[[peer]]
name = "a"
slot = 1
public_key = "{key_a}"
endpoint = "172.31.250.11"
role = "responder"
path_mtu = 1500
prefixes = ["10.77.1.0/24"]
[[rule]]
id = 1
peer = "a"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8080"
"#
            );
            GatewayConfig::parse(&text).unwrap()
        };
        let r = p.b.reconfigure(&cfg, &[(1, [0xB2; 16])], p.now);
        assert_eq!(r.replaced, vec![1]);
        assert!(!p.b.is_established(1));
        p.b.on_exchange(
            &mut p.pb,
            ExchangeRx {
                slot: 1,
                kernel_incarnation: [0xB2; 16],
                bytes: init,
            },
            p.now,
        );
        assert_eq!(
            p.b.counts().duplicates_answered,
            1,
            "a replaced peer must not replay the old peer's RESPONSE"
        );
    }
}
