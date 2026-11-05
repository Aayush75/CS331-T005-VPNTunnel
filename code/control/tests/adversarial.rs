//! RJ5: control-plane adversarial and lifetime tests.
//!
//! Two real `Controller`s talk through a fake kernel (`Port`), so nothing here needs
//! Linux or a kernel module: stale and forged RESPONSEs, duplicate/replayed/conflicting
//! INITs, retry and cache bounds, unilateral outages, and the receive path under
//! `ENOBUFS` and event floods (through the portable `inbox` and its `Source` seam).
//! Linux integration (`tests/netns/ay2_controller.py`) remains the mandatory check.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use vpt_control::config::GatewayConfig;
use vpt_control::exchange::{ResponseCache, SessionKeys};
use vpt_control::genl::inbox::{
    Event, Inbox, MAX_PENDING_EVENTS, Recv, Source, TransportError, run_events,
};
use vpt_control::identity::{PrivateKey, PublicKey};
use vpt_control::netlink_ids::{
    ATTR_EXCHANGE_BYTES, ATTR_PEER_INCARNATION, ATTR_PEER_SLOT, CMD_EXCHANGE_RX,
};
use vpt_control::session::{ATTEMPT_DEADLINE, Controller, Entropy, Log, Port, PortError};
use vpt_control::vptunnel::{ExchangeRx, parse_exchange_rx, request};
use vpt_control::wire::{ATTEMPT_ID_LEN, INCARNATION_LEN, parse_handshake};

const A_INC: [u8; 16] = [0xA1; 16];
const B_INC: [u8; 16] = [0xB1; 16];
/// Same schedule as `session::BACKOFF` (private there): the design's 1, 2, 4, 8, 16, 30 s.
const BACKOFF: [u64; 6] = [1, 2, 4, 8, 16, 30];

// ---- a fake kernel ---------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Staged {
    tx: [u8; 32],
    rx: [u8; 32],
    local: u64,
    remote: u64,
}

#[derive(Default)]
struct Kernel {
    /// Every datagram handed to EXCHANGE_TX, in order.
    sent: Vec<Vec<u8>>,
    staged: Vec<Staged>,
    /// Scripted STAGE_GENERATION failures (errno), consumed one per call.
    stage_errors: VecDeque<i32>,
    /// Every STAGE_GENERATION fails with this errno.
    stage_always: Option<i32>,
    /// EXCHANGE_TX fails (kernel family gone, interface down).
    tx_fails: bool,
    stage_calls: usize,
    incarnation_calls: usize,
    /// STATUS no longer lists the peer.
    peer_gone: bool,
    /// Scripted BEGIN_ATTEMPT failures (errno), consumed one per call.
    begin_errors: VecDeque<i32>,
    /// Every BEGIN_ATTEMPT fails with this errno.
    begin_always: Option<i32>,
    begin_calls: usize,
    /// The kernel's own random source (splitmix64); the controller never sees it.
    prng: u64,
    /// Receive locators issued so far: the kernel never reuses one.
    issued: Vec<u64>,
    /// The kernel's clock, as last told by the test's `Pair`.
    clock: Option<Instant>,
    /// The attempt the kernel issued last and when (valid 10 s, `VPT1_ATTEMPT_NS`).
    pending: Option<([u8; ATTEMPT_ID_LEN], u64, Instant)>,
    /// The attempt ID of the last INIT the kernel delivered, and when.
    delivered_init: Option<([u8; ATTEMPT_ID_LEN], Instant)>,
}

impl Kernel {
    fn next_u64(&mut self) -> u64 {
        self.prng = self.prng.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.prng;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    fn fail(errno: i32, what: &str) -> PortError {
        PortError {
            what: format!("{what} (errno {errno})"),
            errno: Some(errno),
        }
    }

    /// The receive path noted an INIT before the event reached the controller.
    fn note_init(&mut self, bytes: &[u8]) {
        if let (Ok(d), Some(now)) = (parse_handshake(bytes), self.clock) {
            self.delivered_init = Some((d.attempt_id, now));
        }
    }
}

impl Port for Kernel {
    fn begin_attempt(
        &mut self,
        _slot: u8,
        _inc: &[u8; INCARNATION_LEN],
        given: Option<&[u8; ATTEMPT_ID_LEN]>,
    ) -> Result<([u8; ATTEMPT_ID_LEN], u64), PortError> {
        self.begin_calls += 1;
        if let Some(errno) = self.begin_always.or_else(|| self.begin_errors.pop_front()) {
            return Err(Self::fail(errno, "scripted BEGIN_ATTEMPT failure"));
        }
        // ESTALE is scripted by the tests that want it, as with STAGE_GENERATION.
        let now = self.clock.expect("the Pair tells the kernel the time");
        let id = match given {
            // A responder: only the INIT the kernel delivered, and only for 10 s.
            Some(g) => match self.delivered_init {
                Some((d, at)) if d == *g && now.duration_since(at) < ATTEMPT_DEADLINE => *g,
                _ => return Err(Self::fail(2, "no such INIT was delivered")),
            },
            // An initiator: 128 random bits, nonzero.
            None => {
                let mut id = [0u8; ATTEMPT_ID_LEN];
                id[..8].copy_from_slice(&self.next_u64().to_le_bytes());
                id[8..].copy_from_slice(&self.next_u64().to_le_bytes());
                id[0] |= 1;
                id
            }
        };
        let locator = loop {
            let l = self.next_u64();
            if l != 0 && !self.issued.contains(&l) {
                break l;
            }
        };
        self.issued.push(locator);
        self.pending = Some((id, locator, now));
        Ok((id, locator))
    }

    fn rekey_request(
        &mut self,
        _slot: u8,
        _inc: &[u8; INCARNATION_LEN],
        _attempt: &[u8; ATTEMPT_ID_LEN],
    ) -> Result<(), PortError> {
        // This fake kernel never grants continuity, so every re-key here is a fresh attempt.
        Err(PortError {
            what: "no continuity grant in this fake kernel".into(),
            errno: Some(107),
        })
    }

    fn exchange_tx(&mut self, _slot: u8, bytes: &[u8]) -> Result<(), PortError> {
        if self.tx_fails {
            return Err(PortError {
                what: "kernel family gone".into(),
                errno: Some(19),
            });
        }
        self.sent.push(bytes.to_vec());
        Ok(())
    }

    fn stage(
        &mut self,
        _slot: u8,
        _inc: &[u8; INCARNATION_LEN],
        keys: &SessionKeys,
    ) -> Result<(), PortError> {
        self.stage_calls += 1;
        // The kernel stages only the pending attempt's own locator, for 10 s after BEGIN_ATTEMPT.
        if let (Some((_, _, begun)), Some(now)) = (self.pending, self.clock)
            && now.duration_since(begun) >= ATTEMPT_DEADLINE
        {
            return Err(Self::fail(110, "attempt expired"));
        }
        if let Some(errno) = self.stage_always.or_else(|| self.stage_errors.pop_front()) {
            return Err(PortError {
                what: format!("errno {errno}"),
                errno: Some(errno),
            });
        }
        self.staged.push(Staged {
            tx: *keys.tx,
            rx: *keys.rx,
            local: keys.local_locator,
            remote: keys.remote_locator,
        });
        Ok(())
    }

    fn kernel_incarnation(&mut self, _slot: u8) -> Result<[u8; INCARNATION_LEN], PortError> {
        self.incarnation_calls += 1;
        if self.peer_gone {
            return Err(PortError {
                what: "the kernel has no peer in slot 1".into(),
                errno: None,
            });
        }
        Ok([0x77; INCARNATION_LEN])
    }
}

fn config(role: &str, own_net: &str, peer_net: &str, peer: &PublicKey) -> GatewayConfig {
    let (own, other) = if role == "initiator" {
        ("11", "12")
    } else {
        ("12", "11")
    };
    GatewayConfig::parse(&format!(
        r#"
[gateway]
name = "gw"
private_key_file = "/tmp/gw.key"
lan_interface = "lan0"
lan_address = "{own_net}.1/24"
underlay_interface = "out0"
underlay_address = "172.31.250.{own}/24"
[[peer]]
name = "peer"
slot = 1
public_key = "{peer}"
endpoint = "172.31.250.{other}"
role = "{role}"
path_mtu = 1500
prefixes = ["{peer_net}.0/24"]
[[rule]]
id = 1
peer = "peer"
source = "10.77.1.10/32"
destination = "10.77.2.10/32"
protocol = "tcp"
ports = "8080"
"#
    ))
    .unwrap()
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

struct Node {
    ctl: Controller,
    k: Kernel,
    inc: [u8; 16],
}

struct Pair {
    a: Node, // initiator
    b: Node, // responder
    now: Instant,
    logs: Arc<Mutex<Vec<String>>>,
}

fn log(tag: &'static str, logs: &Arc<Mutex<Vec<String>>>) -> Log {
    let logs = logs.clone();
    Box::new(move |m| logs.lock().unwrap().push(format!("{tag}: {m}")))
}

fn pair() -> Pair {
    let ka = PrivateKey::generate().unwrap();
    let kb = PrivateKey::generate().unwrap();
    let cfg_a = config("initiator", "10.77.1", "10.77.2", &kb.public_key());
    let cfg_b = config("responder", "10.77.2", "10.77.1", &ka.public_key());
    let logs = Arc::new(Mutex::new(Vec::new()));
    let now = Instant::now();
    let kernel = |seed: u64| Kernel {
        prng: seed,
        clock: Some(now),
        ..Kernel::default()
    };
    let a = Controller::new(ka, &cfg_a, &[(1, A_INC)], rng(3), log("a", &logs), now);
    let b = Controller::new(kb, &cfg_b, &[(1, B_INC)], rng(9), log("b", &logs), now);
    Pair {
        a: Node {
            ctl: a,
            k: kernel(0xA11CE),
            inc: A_INC,
        },
        b: Node {
            ctl: b,
            k: kernel(0xB0B),
            inc: B_INC,
        },
        now,
        logs,
    }
}

fn rx(node: &Node, bytes: &[u8]) -> ExchangeRx {
    ExchangeRx {
        slot: 1,
        kernel_incarnation: node.inc,
        bytes: bytes.to_vec(),
    }
}

impl Pair {
    /// Tell both fake kernels what time it is before the controllers act.
    fn sync(&mut self) {
        self.a.k.clock = Some(self.now);
        self.b.k.clock = Some(self.now);
    }

    fn tick(&mut self) {
        self.sync();
        self.a.ctl.tick(&mut self.a.k, self.now);
        self.b.ctl.tick(&mut self.b.k, self.now);
    }

    fn advance(&mut self, d: Duration) {
        self.now += d;
        self.tick();
    }

    fn deliver_b(&mut self, bytes: &[u8]) {
        self.sync();
        self.b.k.note_init(bytes);
        let ev = rx(&self.b, bytes);
        self.b.ctl.on_exchange(&mut self.b.k, ev, self.now);
    }

    fn deliver_a(&mut self, bytes: &[u8]) {
        self.sync();
        self.a.k.note_init(bytes);
        let ev = rx(&self.a, bytes);
        self.a.ctl.on_exchange(&mut self.a.k, ev, self.now);
    }

    /// A's first INIT reaches B, B's RESPONSE reaches A.
    fn establish(&mut self) -> (Vec<u8>, Vec<u8>) {
        self.tick();
        let init = self.a.k.sent.last().unwrap().clone();
        self.deliver_b(&init);
        let resp = self.b.k.sent.last().unwrap().clone();
        self.deliver_a(&resp);
        assert!(self.a.ctl.is_established(1) && self.b.ctl.is_established(1));
        (init, resp)
    }

    /// Flip one bit of a datagram.
    fn flip(bytes: &[u8], at: usize) -> Vec<u8> {
        let mut v = bytes.to_vec();
        v[at] ^= 0x01;
        v
    }
}

// ---- 1. stale / late / rejected RESPONSE ---------------------------------------------

#[test]
fn a_replayed_response_after_establishment_changes_nothing() {
    let mut p = pair();
    let (_, resp) = p.establish();
    assert_eq!(p.a.k.staged.len(), 1);
    let rejected = p.a.ctl.counts().rejected;
    for _ in 0..50 {
        p.deliver_a(&resp);
    }
    assert_eq!(p.a.k.stage_calls, 1, "no second install");
    assert_eq!(p.a.ctl.counts().established, 1);
    assert_eq!(p.a.ctl.counts().rejected, rejected + 50);
    assert!(p.a.ctl.is_established(1));
}

#[test]
fn an_old_response_during_a_rekey_does_not_complete_the_new_attempt() {
    let mut p = pair();
    let (_, old_resp) = p.establish();
    assert_eq!(p.a.ctl.request_rekey(), 1);
    p.advance(Duration::from_millis(1)); // the new attempt's INIT leaves, A is pending
    assert!(!p.a.ctl.is_established(1));
    let new_init = p.a.k.sent.last().unwrap().clone();
    p.deliver_a(&old_resp); // late RESPONSE of the previous attempt
    assert_eq!(p.a.k.stage_calls, 1, "the old RESPONSE installed nothing");
    assert!(!p.a.ctl.is_established(1), "and did not end the attempt");
    // The real answer still completes it, exactly once.
    p.deliver_b(&new_init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.deliver_a(&resp);
    p.deliver_a(&resp);
    assert_eq!(p.a.k.staged.len(), 2);
    assert_ne!(p.a.k.staged[0].tx, p.a.k.staged[1].tx);
    assert_eq!(p.a.ctl.counts().established, 2);
}

#[test]
fn a_response_that_arrives_after_the_attempt_expired_installs_nothing() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone(); // held back by the network
    p.advance(ATTEMPT_DEADLINE); // the attempt expires: A is idle, backing off 1 s
    assert_eq!(p.a.ctl.counts().attempts_failed, 1);
    p.advance(Duration::from_millis(500));
    p.deliver_a(&resp);
    assert_eq!(p.a.k.stage_calls, 0);
    assert!(!p.a.ctl.is_established(1));
    // Once the next attempt starts, the same stale RESPONSE has the wrong attempt ID.
    p.advance(Duration::from_millis(600));
    assert_eq!(p.a.ctl.counts().attempts_started, 2);
    p.deliver_a(&resp);
    assert_eq!(p.a.k.stage_calls, 0);
    assert!(!p.a.ctl.is_established(1));
}

#[test]
fn no_single_byte_corruption_of_a_response_is_accepted_and_the_attempt_survives() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    for at in 0..resp.len() {
        let bad = Pair::flip(&resp, at);
        p.deliver_a(&bad);
        assert_eq!(p.a.k.stage_calls, 0, "flip at byte {at} installed keys");
        assert!(!p.a.ctl.is_established(1), "flip at byte {at} completed");
    }
    assert_eq!(p.a.ctl.counts().rejected, resp.len() as u64);
    // Truncations and extensions are rejected too.
    for len in [0, 1, 4, 8, 24, resp.len() - 1, resp.len() + 1] {
        let mut bad = resp.clone();
        bad.resize(len, 0);
        p.deliver_a(&bad);
    }
    assert_eq!(p.a.k.stage_calls, 0);
    // None of that cancelled the attempt: the genuine RESPONSE completes it once.
    p.deliver_a(&resp);
    assert!(p.a.ctl.is_established(1));
    assert_eq!(p.a.k.staged.len(), 1);
}

#[test]
fn a_response_sent_to_a_responder_and_an_init_sent_to_an_initiator_are_ignored() {
    let mut p = pair();
    let (init, resp) = p.establish();
    let (ra, rb) = (p.a.ctl.counts().rejected, p.b.ctl.counts().rejected);
    p.deliver_b(&resp); // B is a responder: never waiting for a RESPONSE
    p.deliver_a(&init); // A is an initiator: never answers an INIT
    assert_eq!(p.b.ctl.counts().rejected, rb + 1);
    assert_eq!(p.a.ctl.counts().rejected, ra + 1);
    assert_eq!((p.a.k.stage_calls, p.b.k.stage_calls), (1, 1));
}

#[test]
fn arbitrary_garbage_never_panics_or_installs_keys() {
    let mut p = pair();
    p.tick();
    for kind in 0..=4u8 {
        for len in (0..=300usize)
            .step_by(7)
            .chain([175, 176, 177, 223, 224, 225])
        {
            let mut v = vec![0xA5u8; len];
            if len > 4 {
                v[..4].copy_from_slice(b"VPT1");
                v[4] = kind;
            }
            p.deliver_a(&v);
            p.deliver_b(&v);
        }
    }
    let unknown = ExchangeRx {
        slot: 9,
        kernel_incarnation: [0; 16],
        bytes: vec![1; 224],
    };
    p.b.ctl.on_exchange(&mut p.b.k, unknown, p.now);
    assert_eq!((p.a.k.stage_calls, p.b.k.stage_calls), (0, 0));
    assert!(!p.a.ctl.is_established(1) && !p.b.ctl.is_established(1));
}

// ---- 2. duplicate, conflicting and replayed INIT --------------------------------------

#[test]
fn an_init_retransmit_storm_installs_one_generation_and_repeats_one_response() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    for _ in 0..500 {
        p.deliver_b(&init);
    }
    assert_eq!(p.b.k.stage_calls, 1);
    assert_eq!(p.b.ctl.counts().established, 1);
    assert_eq!(p.b.ctl.counts().duplicates_answered, 499);
    assert_eq!(p.b.k.sent.len(), 500, "one reply per INIT, nothing more");
    assert!(
        p.b.k.sent.iter().all(|r| *r == p.b.k.sent[0]),
        "the identical RESPONSE"
    );
    // The initiator completes from any copy of it, once.
    let resp = p.b.k.sent[0].clone();
    p.deliver_a(&resp);
    p.deliver_a(&resp);
    assert_eq!(p.a.k.staged.len(), 1);
    assert_eq!(p.a.k.staged[0].tx, p.b.k.staged[0].rx);
}

#[test]
fn a_tampered_init_is_rejected_and_leaves_the_duplicate_cache_intact() {
    let mut p = pair();
    let (init, resp) = p.establish();
    let rejected = p.b.ctl.counts().rejected;
    for at in [4, 8, 16, 40, 100, 200, init.len() - 1] {
        p.deliver_b(&Pair::flip(&init, at));
    }
    assert_eq!(p.b.ctl.counts().rejected, rejected + 7);
    assert_eq!(p.b.k.stage_calls, 1);
    let sent = p.b.k.sent.len();
    p.deliver_b(&init); // the genuine duplicate is still answered from the cache
    assert_eq!(p.b.k.sent.len(), sent + 1);
    assert_eq!(p.b.k.sent.last().unwrap(), &resp);
    assert_eq!(p.b.k.stage_calls, 1);
}

#[test]
fn the_duplicate_cache_expires_at_twenty_seconds_and_then_gives_fresh_keys() {
    let mut p = pair();
    let (init, resp) = p.establish();
    // Literal 20 s on purpose: the test must fail if the constant changes.
    p.advance(Duration::from_millis(19_900));
    p.deliver_b(&init);
    assert_eq!(p.b.ctl.counts().duplicates_answered, 1);
    assert_eq!(p.b.k.stage_calls, 1);
    p.advance(Duration::from_millis(100)); // exactly 20 s after the install
    p.deliver_b(&init);
    assert_eq!(p.b.ctl.counts().duplicates_answered, 1, "cache expired");
    // A replayed old INIT is handled as a new exchange: fresh responder state and locator,
    // never the old keys, and at most one per arrival (the kernel rate-limits arrivals).
    assert_eq!(p.b.k.stage_calls, 2);
    let fresh = p.b.k.sent.last().unwrap();
    assert_ne!(
        fresh, &resp,
        "fresh responder ephemeral, so a different RESPONSE"
    );
    assert_ne!(p.b.k.staged[0].tx, p.b.k.staged[1].tx);
    assert_ne!(p.b.k.staged[0].rx, p.b.k.staged[1].rx);
    assert_ne!(p.b.k.staged[0].local, p.b.k.staged[1].local);
}

#[test]
fn a_conflicting_second_attempt_is_a_new_exchange_and_the_first_is_not_replayed_from_cache() {
    let mut p = pair();
    let (init1, _) = p.establish();
    assert_eq!(p.a.ctl.request_rekey(), 1);
    p.advance(Duration::from_millis(1));
    let init2 = p.a.k.sent.last().unwrap().clone();
    assert_ne!(
        init1, init2,
        "a new attempt has a new attempt ID and ephemeral"
    );
    p.deliver_b(&init2);
    assert_eq!(p.b.k.stage_calls, 2);
    // The cache now holds attempt 2 only. Attempt 1's INIT is not a duplicate any more.
    p.deliver_b(&init2);
    assert_eq!(p.b.ctl.counts().duplicates_answered, 1);
    p.deliver_b(&init1);
    assert_eq!(p.b.ctl.counts().duplicates_answered, 1);
    // It never re-installs the generation-1 keys: every install is fresh material.
    let all: Vec<_> = p.b.k.staged.iter().flat_map(|s| [s.tx, s.rx]).collect();
    for (i, a) in all.iter().enumerate() {
        for b in &all[i + 1..] {
            assert_ne!(a, b, "a key was installed twice");
        }
    }
}

#[test]
fn the_response_cache_is_exact_and_one_entry_per_peer_by_construction() {
    let init = vec![7u8; 224];
    let resp = vec![9u8; 176];
    let cache = ResponseCache::new(&init, &resp);
    assert_eq!(cache.replay_for(&init), Some(&resp[..]));
    for at in 0..init.len() {
        assert_eq!(
            cache.replay_for(&Pair::flip(&init, at)),
            None,
            "bit flip at {at}"
        );
    }
    for len in [0, 1, 223, 225, 448] {
        let mut longer = init.clone();
        longer.resize(len, 7);
        assert_eq!(cache.replay_for(&longer), None, "length {len}");
    }
    // Sizes never depend on the number of INITs seen: the controller replaces the single
    // per-peer entry on every new exchange (`PeerCtl::cached: Option<Cached>`).
    let mut p = pair();
    p.tick();
    for _ in 0..200 {
        p.a.ctl.request_rekey();
        p.advance(Duration::from_secs(11));
        let init = p.a.k.sent.last().unwrap().clone();
        p.deliver_b(&init);
    }
    // Each distinct INIT replaced the entry; only the latest is a duplicate.
    let last = p.a.k.sent.last().unwrap().clone();
    let dup = p.b.ctl.counts().duplicates_answered;
    p.deliver_b(&last);
    assert_eq!(p.b.ctl.counts().duplicates_answered, dup + 1);
}

// ---- 3. retry and backoff bounds ------------------------------------------------------

/// Run a peer that never answers; return the offsets (ms) at which INITs left.
fn silent_peer_sends(seconds: u64) -> (Pair, Vec<u64>) {
    let mut p = pair();
    let t0 = p.now;
    let mut at = Vec::new();
    let mut seen = 0;
    for _ in 0..seconds * 10 {
        p.tick();
        while seen < p.a.k.sent.len() {
            at.push(p.now.duration_since(t0).as_millis() as u64);
            seen += 1;
        }
        p.now += Duration::from_millis(100);
    }
    (p, at)
}

#[test]
fn a_silent_peer_gets_five_inits_per_attempt_and_the_backoff_schedule() {
    let (p, sends) = silent_peer_sends(300);
    // Attempt 1: INIT at 0, then retransmits 0.5, 1.5, 3.5, 7.5 s (0.5/1/2/4 s steps).
    assert_eq!(&sends[..5], &[0, 500, 1500, 3500, 7500]);
    // Attempts start every 10 s + backoff (1, 2, 4, 8, 16, then 30 s forever), within one tick.
    let starts: Vec<u64> = sends
        .iter()
        .copied()
        .enumerate()
        .filter(|(i, _)| i % 5 == 0)
        .map(|(_, t)| t)
        .collect();
    let mut expect = 0u64;
    for (k, s) in starts.iter().enumerate() {
        assert!(
            s.abs_diff(expect) <= 200,
            "attempt {k} started at {s} ms, expected about {expect} ms"
        );
        expect = s + 10_000 + 1000 * BACKOFF[k.min(BACKOFF.len() - 1)];
    }
    assert!(starts.len() >= 8, "{starts:?}");
    // Whole run: every attempt is exactly 5 datagrams, no growth beyond that.
    assert_eq!(sends.len() % 5, 0, "{sends:?}");
    assert_eq!(sends.len() as u64, 5 * p.a.ctl.counts().attempts_started);
    // 300 s of outage is bounded by the schedule: 10+1, 10+2, 10+4, 10+8, 10+16, then 40 s.
    assert!(
        p.a.ctl.counts().attempts_started <= 12,
        "{}",
        p.a.ctl.counts().attempts_started
    );
    assert_eq!(p.a.k.stage_calls, 0);
}

#[test]
fn the_pending_attempt_never_outlives_its_deadline_and_state_does_not_accumulate() {
    let (p, _) = silent_peer_sends(120);
    let c = p.a.ctl.counts();
    // Every started attempt except possibly the current one has expired.
    assert!(c.attempts_failed + 1 >= c.attempts_started, "{c:?}");
    assert_eq!(c.rejected, 0);
    assert!(
        p.logs.lock().unwrap().len() < 2 * (c.attempts_started as usize) + 4,
        "log growth"
    );
}

#[test]
fn a_success_resets_the_backoff_to_one_second() {
    let mut p = pair();
    p.tick();
    p.advance(ATTEMPT_DEADLINE); // failure 1, back off 1 s
    p.advance(Duration::from_millis(1100)); // attempt 2 starts
    p.advance(ATTEMPT_DEADLINE); // failure 2, back off 2 s
    p.advance(Duration::from_millis(2100)); // attempt 3 starts and succeeds
    assert_eq!(p.a.ctl.counts().attempts_started, 3);
    let init = p.a.k.sent.last().unwrap().clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.deliver_a(&resp);
    assert!(p.a.ctl.is_established(1));
    // A later failure starts from the first step again (1 s), not the third (4 s).
    p.a.ctl.request_rekey();
    p.advance(Duration::from_millis(1));
    let started = p.a.ctl.counts().attempts_started;
    p.advance(ATTEMPT_DEADLINE);
    p.advance(Duration::from_millis(1100));
    assert_eq!(p.a.ctl.counts().attempts_started, started + 1);
}

#[test]
fn a_failing_exchange_tx_backs_off_instead_of_spinning() {
    let mut p = pair();
    p.a.k.tx_fails = true;
    let t0 = p.now;
    let mut calls = 0;
    for _ in 0..3000 {
        p.tick();
        p.now += Duration::from_millis(100);
        calls = p.a.ctl.counts().attempts_failed;
    }
    // 300 s: failures at 0, 1, 3, 7, 15, 31, 61, 91, ... never one per tick.
    assert!(
        (10..=15).contains(&calls),
        "{calls} failures in {:?}",
        p.now - t0
    );
    assert_eq!(p.a.ctl.counts().attempts_started, 0);
    p.a.k.tx_fails = false;
    p.advance(Duration::from_secs(31));
    assert_eq!(
        p.a.ctl.counts().attempts_started,
        1,
        "recovers once the kernel is back"
    );
}

#[test]
fn a_hundred_rekey_requests_start_one_exchange() {
    let mut p = pair();
    p.establish();
    for _ in 0..100 {
        p.a.ctl.request_rekey();
    }
    p.advance(Duration::from_millis(1));
    assert_eq!(p.a.ctl.counts().attempts_started, 2);
    p.advance(Duration::from_millis(100));
    assert_eq!(
        p.a.ctl.counts().attempts_started,
        2,
        "pending attempts are not restarted"
    );
    assert_eq!(
        p.a.ctl.request_rekey(),
        0,
        "a pending peer is not 'established'"
    );
}

// ---- 4. unilateral outage -------------------------------------------------------------

#[test]
fn a_peer_that_vanishes_after_establishment_is_retried_without_growth() {
    let mut p = pair();
    p.establish();
    // The peer is gone: nothing it would send ever arrives. A re-keys at 90 s and then
    // retries on the backoff schedule; it never reports the tunnel up again.
    let t0 = p.now;
    let mut max_inits_per_minute = 0usize;
    let mut last_inits_per_minute = 0usize;
    let mut window = (p.now, p.a.k.sent.len());
    for _ in 0..6000 {
        p.now += Duration::from_millis(100);
        p.tick();
        if p.now.duration_since(window.0) >= Duration::from_secs(60) {
            last_inits_per_minute = p.a.k.sent.len() - window.1;
            max_inits_per_minute = max_inits_per_minute.max(last_inits_per_minute);
            window = (p.now, p.a.k.sent.len());
        }
    }
    assert!(p.now.duration_since(t0) >= Duration::from_secs(600));
    assert!(!p.a.ctl.is_established(1));
    assert_eq!(
        p.a.k.stage_calls, 1,
        "no key was installed while the peer was silent"
    );
    // Worst case is the early part of the schedule (attempts 10+1, 10+2, 10+4, 10+8 s apart:
    // five per minute at five INITs each); in the steady state (40 s per attempt) it is two.
    assert!(
        max_inits_per_minute <= 25,
        "{max_inits_per_minute} INITs in one minute"
    );
    assert!(
        last_inits_per_minute <= 10,
        "{last_inits_per_minute} INITs in the last minute"
    );
}

#[test]
fn a_responder_whose_initiator_vanishes_stays_quiet() {
    let mut p = pair();
    p.establish();
    let sent = p.b.k.sent.len();
    for _ in 0..1200 {
        p.advance(Duration::from_millis(500));
    }
    // A responder never originates traffic and has no timer that creates state.
    assert_eq!(p.b.k.sent.len(), sent);
    assert_eq!(p.b.k.stage_calls, 1);
}

#[test]
fn a_lost_lease_fails_the_install_and_the_attempt_backs_off_with_no_key_left_staged() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.a.k.stage_always = Some(1); // EPERM: another controller owns the gateway now
    p.deliver_a(&resp);
    assert_eq!(
        p.a.k.stage_calls, 1,
        "EPERM is not retried like a stale incarnation"
    );
    assert_eq!(p.a.k.incarnation_calls, 0);
    assert!(!p.a.ctl.is_established(1));
    assert_eq!(p.a.ctl.counts().attempts_failed, 1);
    p.deliver_a(&resp); // the late duplicate cannot resurrect the attempt
    assert_eq!(p.a.k.stage_calls, 1);
    // A new attempt starts after 1 s and is also refused; still no loop.
    p.advance(Duration::from_millis(1100));
    assert_eq!(p.a.ctl.counts().attempts_started, 2);
    assert!(p.a.k.staged.is_empty());
}

#[test]
fn a_stale_incarnation_is_refreshed_once_not_in_a_loop() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.a.k.stage_always = Some(116); // ESTALE every time
    p.deliver_a(&resp);
    assert_eq!(p.a.k.incarnation_calls, 1);
    assert_eq!(
        p.a.k.stage_calls, 2,
        "one retry with the refreshed incarnation, then give up"
    );
    assert!(!p.a.ctl.is_established(1));
}

// ---- BEGIN_ATTEMPT: the kernel issues the attempt ID and the locator ------------------

#[test]
fn the_kernel_issues_random_looking_nonzero_ids_and_distinct_locators() {
    let mut p = pair();
    p.establish();
    let (id1, loc1, _) = p.a.k.pending.unwrap();
    assert!(id1.iter().any(|b| *b != 0));
    assert_ne!(loc1, 0);
    p.a.ctl.request_rekey();
    p.advance(Duration::from_millis(1));
    let (id2, loc2, _) = p.a.k.pending.unwrap();
    assert_ne!(id1, id2, "a new attempt gets a new ID");
    assert_ne!(loc1, loc2, "the kernel never reuses a locator");
    // The responder's locator is also the kernel's, and never the initiator's.
    assert_eq!(p.b.k.begin_calls, 1);
    assert!(
        p.b.k
            .issued
            .iter()
            .all(|l| *l != 0 && !p.a.k.issued.contains(l))
    );
}

#[test]
fn a_stale_incarnation_at_begin_attempt_is_refreshed_once_and_the_init_still_leaves() {
    let mut p = pair();
    p.a.k.begin_errors.push_back(116); // ESTALE once
    p.tick();
    assert_eq!(
        p.a.k.begin_calls, 2,
        "one retry with the refreshed incarnation"
    );
    assert_eq!(p.a.k.incarnation_calls, 1);
    assert_eq!(p.a.k.sent.len(), 1, "the INIT left on the retry");
    assert_eq!(p.a.ctl.counts().attempts_started, 1);
    assert_eq!(p.a.ctl.counts().attempts_failed, 0);
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.deliver_a(&resp);
    assert!(p.a.ctl.is_established(1));
}

#[test]
fn a_persistently_stale_begin_attempt_sends_no_init_and_backs_off() {
    let mut p = pair();
    p.a.k.begin_always = Some(116);
    p.tick();
    assert_eq!(
        p.a.k.begin_calls, 2,
        "refresh once, retry once, then give up"
    );
    assert_eq!(p.a.k.incarnation_calls, 1);
    assert!(
        p.a.k.sent.is_empty(),
        "no INIT without a kernel-issued attempt"
    );
    assert_eq!(p.a.ctl.counts().attempts_started, 0);
    assert_eq!(p.a.ctl.counts().attempts_failed, 1);
    // No spinning: nothing happens before the 1 s backoff is over, then one more try.
    let calls = p.a.k.begin_calls;
    p.advance(Duration::from_millis(900));
    assert_eq!(p.a.k.begin_calls, calls);
    p.advance(Duration::from_millis(200));
    assert_eq!(p.a.k.begin_calls, calls + 2);
    assert_eq!(p.a.ctl.counts().attempts_failed, 2);
    // The second failure backs off 2 s.
    p.advance(Duration::from_millis(1900));
    assert_eq!(p.a.k.begin_calls, calls + 2);
}

#[test]
fn a_begin_attempt_failure_that_is_not_stale_is_not_retried() {
    let mut p = pair();
    p.a.k.begin_errors.push_back(12); // ENOMEM
    p.tick();
    assert_eq!(p.a.k.begin_calls, 1);
    assert_eq!(p.a.k.incarnation_calls, 0);
    assert!(p.a.k.sent.is_empty());
    assert_eq!(p.a.ctl.counts().attempts_failed, 1);
}

#[test]
fn a_peer_missing_from_status_ends_begin_attempt_without_a_retry() {
    let mut p = pair();
    p.a.k.begin_errors.push_back(116);
    p.a.k.peer_gone = true;
    p.tick();
    assert_eq!(p.a.k.begin_calls, 1);
    assert_eq!(p.a.k.incarnation_calls, 1);
    assert!(p.a.k.sent.is_empty());
    assert_eq!(p.a.ctl.counts().attempts_failed, 1);
}

#[test]
fn an_init_the_kernel_never_delivered_gets_enoent_and_no_response() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    // The event reaches the controller, but the kernel's receive path noted no such INIT.
    p.sync();
    let ev = rx(&p.b, &init);
    p.b.ctl.on_exchange(&mut p.b.k, ev, p.now);
    assert_eq!(p.b.k.begin_calls, 1);
    assert_eq!(p.b.k.stage_calls, 0);
    assert!(
        p.b.k.sent.is_empty(),
        "no RESPONSE for an INIT the kernel did not hand over"
    );
    assert!(!p.b.ctl.is_established(1));
    assert_eq!(p.b.ctl.counts().rejected, 1);
    assert_eq!(p.b.ctl.counts().established, 0);
    assert!(
        p.logs
            .lock()
            .unwrap()
            .iter()
            .any(|l| l.contains("cannot begin an attempt")),
        "the refusal is logged"
    );
    // The genuine path still works: the retransmit arrives through the receive path.
    p.deliver_b(&init);
    assert_eq!(p.b.k.sent.len(), 1);
    assert!(p.b.ctl.is_established(1));
}

#[test]
fn an_init_older_than_ten_seconds_at_the_kernel_is_refused_for_a_responder() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.sync();
    p.b.k.note_init(&init);
    p.now += ATTEMPT_DEADLINE; // the controller got to the event ten seconds late
    p.sync();
    let ev = rx(&p.b, &init);
    p.b.ctl.on_exchange(&mut p.b.k, ev, p.now);
    assert!(p.b.k.sent.is_empty());
    assert_eq!(p.b.k.stage_calls, 0);
    assert!(!p.b.ctl.is_established(1));
    assert_eq!(p.b.ctl.counts().rejected, 1);
}

#[test]
fn a_responder_only_answers_the_init_the_kernel_delivered_last() {
    let mut p = pair();
    p.tick();
    let first = p.a.k.sent[0].clone();
    // A second, different attempt's INIT replaces the first in the kernel's memory.
    p.advance(ATTEMPT_DEADLINE);
    p.advance(Duration::from_millis(1100));
    let second = p.a.k.sent.last().unwrap().clone();
    assert_ne!(first, second);
    p.deliver_b(&second);
    assert!(p.b.ctl.is_established(1));
    let begins = p.b.k.begin_calls;
    // The old INIT now arrives as an event the kernel did not note: refused, nothing more staged.
    p.sync();
    let ev = rx(&p.b, &first);
    p.b.ctl.on_exchange(&mut p.b.k, ev, p.now);
    assert_eq!(p.b.k.begin_calls, begins + 1);
    assert_eq!(p.b.k.staged.len(), 1);
    assert_eq!(p.b.k.sent.len(), 1);
}

#[test]
fn a_response_after_the_ten_second_attempt_deadline_is_not_staged() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    // Past the deadline, with the controller not yet ticked, the kernel refuses the install.
    p.now += ATTEMPT_DEADLINE;
    p.deliver_a(&resp);
    assert_eq!(
        p.a.k.staged.len(),
        0,
        "nothing is staged after the deadline"
    );
    assert!(!p.a.ctl.is_established(1));
    assert_eq!(p.a.ctl.counts().established, 0);
    // The next tick expires the attempt as usual and a fresh attempt follows.
    p.advance(Duration::from_millis(1));
    p.advance(Duration::from_millis(1100));
    assert!(p.a.ctl.counts().attempts_started >= 2);
    assert_eq!(p.a.k.staged.len(), 0);
}

#[test]
fn a_response_just_inside_the_ten_second_deadline_is_staged() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.now += ATTEMPT_DEADLINE - Duration::from_millis(1);
    p.deliver_a(&resp);
    assert_eq!(p.a.k.staged.len(), 1);
    assert!(p.a.ctl.is_established(1));
}

#[test]
fn a_peer_missing_from_status_ends_the_install_without_a_retry() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.deliver_b(&init);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.a.k.stage_errors.push_back(116);
    p.a.k.peer_gone = true;
    p.deliver_a(&resp);
    assert_eq!(p.a.k.stage_calls, 1);
    assert_eq!(p.a.k.incarnation_calls, 1);
    assert!(!p.a.ctl.is_established(1));
}

#[test]
fn a_responder_that_cannot_stage_sends_no_response_and_recovers_on_the_retransmit() {
    let mut p = pair();
    p.tick();
    let init = p.a.k.sent[0].clone();
    p.b.k.stage_errors.push_back(1);
    p.deliver_b(&init);
    assert!(
        p.b.k.sent.is_empty(),
        "a RESPONSE must not leave before the keys are in"
    );
    assert!(!p.b.ctl.is_established(1));
    // Nothing was cached, so the retransmitted INIT gets a fresh attempt and now succeeds.
    p.deliver_b(&init);
    assert_eq!(p.b.k.sent.len(), 1);
    assert_eq!(p.b.k.staged.len(), 1);
    assert_eq!(p.b.ctl.counts().duplicates_answered, 0);
    let resp = p.b.k.sent[0].clone();
    p.deliver_a(&resp);
    assert_eq!(p.a.k.staged[0].tx, p.b.k.staged[0].rx);
}

// ---- 4b. the kernel event path under ENOBUFS and floods (Source/inbox seams) ----------

enum Step {
    Kernel(Vec<u8>),
    Foreign(Vec<u8>),
    Overrun,
}

struct Script {
    steps: VecDeque<Step>,
    current: Vec<u8>,
}

impl Source for Script {
    fn recv(&mut self, _t: Duration) -> Result<Recv<'_>, TransportError> {
        let from_kernel = match self.steps.pop_front() {
            None => return Ok(Recv::TimedOut),
            Some(Step::Overrun) => return Ok(Recv::Overrun),
            Some(Step::Kernel(b)) => {
                self.current = b;
                true
            }
            Some(Step::Foreign(b)) => {
                self.current = b;
                false
            }
        };
        Ok(Recv::Datagram {
            bytes: &self.current,
            from_kernel,
        })
    }
}

fn script(steps: Vec<Step>) -> Script {
    Script {
        steps: steps.into(),
        current: Vec::new(),
    }
}

/// One EXCHANGE_RX event datagram as the kernel unicasts it to the owner (sequence 0).
fn exchange_rx_event(slot: u16, inc: [u8; 16], bytes: &[u8]) -> Vec<u8> {
    let mut r = request(0x30, CMD_EXCHANGE_RX, 7, None).unwrap();
    r.put_u16(ATTR_PEER_SLOT, slot).unwrap();
    r.put_bytes(ATTR_PEER_INCARNATION, &inc).unwrap();
    r.put_bytes(ATTR_EXCHANGE_BYTES, bytes).unwrap();
    r.finish(0).to_vec()
}

/// What `daemon::session` does with the receive path, minus the sockets.
fn pump(p: &mut Pair, src: &mut Script, inbox: &mut Inbox) -> usize {
    let mut handled = 0;
    while let Some(Event { payload, .. }) =
        run_events(src, inbox, Duration::from_millis(10)).expect("recv")
    {
        let ev = parse_exchange_rx(&payload).expect("a well-formed event");
        p.sync();
        p.b.k.note_init(&ev.bytes);
        p.b.ctl.on_exchange(&mut p.b.k, ev, p.now);
        handled += 1;
    }
    handled
}

#[test]
fn enobufs_loses_the_init_and_the_retransmit_heals_it_without_a_second_install() {
    let mut p = pair();
    let mut inbox = Inbox::new(4242);
    p.tick();
    let init = p.a.k.sent[0].clone();
    // The kernel reports it dropped events for this socket: the INIT never reached us.
    let mut src = script(vec![Step::Overrun]);
    assert_eq!(pump(&mut p, &mut src, &mut inbox), 0);
    assert!(
        inbox.take_events_lost(),
        "the controller is told events were lost"
    );
    assert!(!inbox.take_events_lost(), "the flag is read once");
    assert_eq!(p.b.k.stage_calls, 0);
    // A's retransmit at 0.5 s arrives.
    p.advance(Duration::from_millis(500));
    let again = p.a.k.sent.last().unwrap().clone();
    assert_eq!(again, init, "the same bytes");
    let mut src = script(vec![Step::Kernel(exchange_rx_event(1, B_INC, &again))]);
    assert_eq!(pump(&mut p, &mut src, &mut inbox), 1);
    let resp = p.b.k.sent.last().unwrap().clone();
    p.deliver_a(&resp);
    assert!(p.a.ctl.is_established(1) && p.b.ctl.is_established(1));
    assert_eq!((p.a.k.stage_calls, p.b.k.stage_calls), (1, 1));
}

#[test]
fn an_event_flood_is_bounded_and_installs_one_generation() {
    let mut p = pair();
    let mut inbox = Inbox::new(4242);
    p.tick();
    let init = p.a.k.sent[0].clone();
    // 200 INIT events in a single datagram: the inbox keeps at most MAX_PENDING_EVENTS.
    let one = exchange_rx_event(1, B_INC, &init);
    let mut src = script(vec![Step::Kernel(one.repeat(200))]);
    let handled = pump(&mut p, &mut src, &mut inbox);
    assert_eq!(handled, MAX_PENDING_EVENTS);
    assert_eq!(
        inbox.stats().events_overflowed,
        200 - MAX_PENDING_EVENTS as u64
    );
    assert!(inbox.take_events_lost());
    assert_eq!(inbox.pending_events(), 0);
    assert_eq!(p.b.k.stage_calls, 1);
    assert_eq!(p.b.ctl.counts().duplicates_answered, handled as u64 - 1);
}

#[test]
fn a_datagram_that_did_not_come_from_the_kernel_never_reaches_the_controller() {
    let mut p = pair();
    let mut inbox = Inbox::new(4242);
    p.tick();
    let init = p.a.k.sent[0].clone();
    let mut src = script(vec![Step::Foreign(exchange_rx_event(1, B_INC, &init))]);
    assert_eq!(pump(&mut p, &mut src, &mut inbox), 0);
    assert_eq!(inbox.stats().foreign_datagrams, 1);
    assert_eq!(p.b.k.stage_calls, 0);
}

// ---- 5. raw-key hygiene (see docs/CONTROLLER.md "Raw-key audit") ----------------------

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

#[test]
fn session_keys_are_zeroizing_redacted_and_never_logged() {
    use zeroize::Zeroizing;
    // Compile-time: the key fields are `Zeroizing`, which erases them on drop.
    fn zeroizing(_: &Zeroizing<[u8; 32]>) {}

    let ka = PrivateKey::generate().unwrap();
    let kb = PrivateKey::generate().unwrap();
    let a_secret = hex(ka.expose_secret());
    let mut p = pair();
    p.establish();
    for _ in 0..3 {
        p.a.ctl.request_rekey();
        p.advance(Duration::from_millis(1));
        let init = p.a.k.sent.last().unwrap().clone();
        p.deliver_b(&init);
        let resp = p.b.k.sent.last().unwrap().clone();
        p.deliver_a(&resp);
    }
    assert_eq!(p.a.k.staged.len(), 4);
    let logs = p.logs.lock().unwrap().join("\n").to_lowercase();
    for s in p.a.k.staged.iter().chain(p.b.k.staged.iter()) {
        for key in [&s.tx, &s.rx] {
            assert!(
                !logs.contains(&hex(key)),
                "a session key appeared in a log line"
            );
            assert!(
                !logs.contains(&hex(&key[..8])),
                "a key prefix appeared in a log line"
            );
        }
    }
    assert!(!logs.contains(&a_secret));
    assert!(!format!("{ka:?}{kb:?}").contains(&a_secret));

    // Debug of the keys redacts both directions.
    let init_key = PrivateKey::generate().unwrap();
    let resp_key = PrivateKey::generate().unwrap();
    let mut ini = vpt_control::exchange::Initiator::start(
        &init_key,
        &resp_key.public_key(),
        vpt_control::wire::HandshakePayload {
            mode: vpt_control::wire::Mode::Fresh,
            attempt_id: [3; 16],
            incarnation: [4; 16],
            initiator_locator: 5,
            responder_locator: 0,
            parent: [0; 32],
            path_budget: 1500,
        },
    )
    .unwrap();
    let r = vpt_control::exchange::respond(
        &resp_key,
        &init_key.public_key(),
        ini.init_bytes(),
        &vpt_control::exchange::ResponderParams {
            incarnation: [6; 16],
            responder_locator: 7,
            path_budget: 1500,
            continuity_grant: None,
        },
    )
    .unwrap();
    zeroizing(&r.keys.tx);
    zeroizing(&r.keys.rx);
    let shown = format!("{r:?} {:?}", r.keys);
    assert!(!shown.contains(&hex(&*r.keys.tx)) && !shown.contains(&hex(&*r.keys.rx)));
    assert!(shown.contains("<redacted>"));
    // The initiator's keys equal the responder's mirrored ones and are redacted the same way.
    let ki = ini.finish(&r.response).unwrap();
    zeroizing(&ki.tx);
    assert_eq!(*ki.tx, *r.keys.rx);
    assert!(!format!("{ki:?}").contains(&hex(&*ki.tx)));
    // The one-time handoff cannot be repeated: a second finish is refused.
    assert!(ini.finish(&r.response).is_err());
}

#[test]
fn raw_split_and_key_exposure_stay_confined_to_the_audited_call_sites() {
    let count = |haystack: &str, needle: &str| haystack.matches(needle).count();
    let exchange = include_str!("../src/exchange.rs");
    let session = include_str!("../src/session.rs");
    let daemon = include_str!("../src/daemon.rs");
    let client = include_str!("../src/client.rs");
    let vptunnel = include_str!("../src/vptunnel.rs");
    let identity = include_str!("../src/identity.rs");
    let all = [exchange, session, daemon, client, vptunnel, identity];
    // The raw Split is taken in exactly one place.
    assert_eq!(
        all.iter()
            .map(|s| count(s, "dangerously_get_raw_split("))
            .sum::<usize>(),
        1
    );
    assert_eq!(count(exchange, "dangerously_get_raw_split("), 1);
    // The static key is exposed only to feed snow in INIT/RESPONSE construction.
    assert_eq!(count(exchange, "expose_secret()"), 2);
    // Production code only: the unit tests in session.rs read keys on purpose.
    let session = session.split("#[cfg(test)]").next().unwrap();
    for (name, src) in [("session", session), ("daemon", daemon), ("client", client)] {
        assert_eq!(
            count(src, "expose_secret"),
            0,
            "{name} must not read the identity"
        );
        assert_eq!(count(src, "dangerously_get_raw_split"), 0, "{name}");
        // Neither a Debug-print of keys nor a format of the tx/rx fields.
        assert!(
            !src.contains("keys.tx") || name == "client",
            "{name} touches keys.tx"
        );
        assert!(
            !src.contains("{keys:?}") && !src.contains("{responded:?}"),
            "{name}"
        );
    }
    // Key bytes enter the kernel request in exactly one function, whose buffer is Zeroizing.
    assert_eq!(count(vptunnel, "keys.tx["), 1);
    assert_eq!(count(vptunnel, "keys.rx["), 1);
    // Only the process logger writes to stderr, and `SessionKeys` has no Clone/Copy.
    assert_eq!(count(daemon, "eprintln!"), 1);
    let before_session_keys = exchange.split("pub struct SessionKeys").next().unwrap();
    let derive_line = before_session_keys.lines().last().unwrap_or("");
    assert!(
        !derive_line.contains("Clone") && !derive_line.contains("Copy"),
        "{derive_line}"
    );
}
