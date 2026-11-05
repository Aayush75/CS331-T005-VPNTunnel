//! Re-keying because a key generation has carried a lot of traffic (AY4).
//!
//! The kernel stops a generation at 1 GiB or 1,048,576 records in either direction and calls 512 MiB or
//! 524,288 records the soft limit. A busy link reaches those long before the 90 second age timer: a
//! saturated 100 Mbit/s cable moves 1 GiB in about 86 seconds. The controller therefore watches the
//! tunnel device's own byte and packet counters (`/sys/class/net/<device>/statistics`, no Netlink
//! change) and asks for a new exchange once either direction has carried [`REKEY_BYTES`] or
//! [`REKEY_RECORDS`] since the current keys were installed, well before the soft limit.
//!
//! The counters belong to the device, not to one peer, so with several peers one peer's keys can be
//! replaced somewhat late; the gap between the soft and hard limits absorbs that. Only the initiator
//! can start an exchange; it sees the responder's traffic as its own received counters.

use std::time::{Duration, Instant};

/// Re-key at about 60% of the 512 MiB soft byte limit.
pub const REKEY_BYTES: u64 = 300 << 20;
/// Re-key at about 57% of the 524,288 record soft limit.
pub const REKEY_RECORDS: u64 = 300_000;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counters {
    pub tx_bytes: u64,
    pub rx_bytes: u64,
    pub tx_packets: u64,
    pub rx_packets: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Limits {
    pub bytes: u64,
    pub records: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            bytes: REKEY_BYTES,
            records: REKEY_RECORDS,
        }
    }
}

impl Counters {
    /// The device's statistics, or `None` if it has none (not Linux, or no such device).
    pub fn read(device: &str) -> Option<Counters> {
        let get = |name: &str| -> Option<u64> {
            std::fs::read_to_string(format!("/sys/class/net/{device}/statistics/{name}"))
                .ok()?
                .trim()
                .parse()
                .ok()
        };
        Some(Counters {
            tx_bytes: get("tx_bytes")?,
            rx_bytes: get("rx_bytes")?,
            tx_packets: get("tx_packets")?,
            rx_packets: get("rx_packets")?,
        })
    }

    /// Has either direction carried at least a limit since `base`? A counter that went backwards
    /// (the device was recreated) counts as no usage.
    pub fn exceeds(&self, base: &Counters, limits: &Limits) -> bool {
        let d = |now: u64, then: u64| now.saturating_sub(then);
        d(self.tx_bytes, base.tx_bytes) >= limits.bytes
            || d(self.rx_bytes, base.rx_bytes) >= limits.bytes
            || d(self.tx_packets, base.tx_packets) >= limits.records
            || d(self.rx_packets, base.rx_packets) >= limits.records
    }
}

/// Decides when to ask for a new exchange. Fed once a second with the counters and the number of
/// exchanges established so far.
pub struct Monitor {
    limits: Limits,
    base: Option<Counters>,
    seen_established: u64,
    requested: bool,
}

impl Monitor {
    pub fn new(limits: Limits) -> Self {
        Monitor {
            limits,
            base: None,
            seen_established: 0,
            requested: false,
        }
    }

    /// True once per generation, when usage has reached a limit and a re-key should start. A new
    /// establishment (the count changed) starts a fresh baseline, so usage is per generation.
    pub fn update(&mut self, now: Counters, established: u64) -> bool {
        if self.base.is_none() || established != self.seen_established {
            self.base = Some(now);
            self.seen_established = established;
            self.requested = false;
            return false;
        }
        if !self.requested && self.base.is_some_and(|b| now.exceeds(&b, &self.limits)) {
            self.requested = true;
            return true;
        }
        false
    }
}

// ---- A silent peer ---------------------------------------------------------------------------

/// How long nothing may arrive from the peer while we keep sending before new keys are tried.
pub const SILENCE: Duration = Duration::from_secs(6);
/// At least this many records must have been sent in that time, so an idle tunnel is left alone.
pub const SILENCE_MIN_SENT: u64 = 5;
/// The shortest time between two such re-keys.
pub const SILENCE_COOLDOWN: Duration = Duration::from_secs(15);

#[derive(Clone, Copy)]
struct Base {
    at: Instant,
    tx_packets: u64,
    rx_packets: u64,
}

/// Notices a peer that stopped answering, for example one that was restarted and no longer holds
/// our keys, so the initiator does not wait for the 90 second age timer. Fed once a second with the
/// device counters, like [`Monitor`]. The signal is: records keep leaving, nothing comes back.
///
/// Limits: only the initiator can start an exchange, so this helps only there, and only when it
/// has traffic of its own to send. One-way traffic that is never answered (a UDP stream with no
/// reply) looks the same and causes one cheap re-key every [`SILENCE_COOLDOWN`].
pub struct Silence {
    base: Option<Base>,
    seen_established: u64,
    last_trigger: Option<Instant>,
}

impl Default for Silence {
    fn default() -> Self {
        Self::new()
    }
}

impl Silence {
    pub fn new() -> Self {
        Silence {
            base: None,
            seen_established: 0,
            last_trigger: None,
        }
    }

    /// True when a re-key should start now.
    pub fn update(&mut self, now: Instant, c: Counters, established: u64) -> bool {
        let fresh = Base {
            at: now,
            tx_packets: c.tx_packets,
            rx_packets: c.rx_packets,
        };
        let Some(base) = self.base else {
            self.base = Some(fresh);
            self.seen_established = established;
            return false;
        };
        // New keys, something received, or a device that was recreated: start counting again.
        if established != self.seen_established
            || c.rx_packets != base.rx_packets
            || c.tx_packets < base.tx_packets
        {
            self.base = Some(fresh);
            self.seen_established = established;
            return false;
        }
        let cooled = self
            .last_trigger
            .is_none_or(|t| now.duration_since(t) >= SILENCE_COOLDOWN);
        if now.duration_since(base.at) >= SILENCE
            && c.tx_packets - base.tx_packets >= SILENCE_MIN_SENT
            && cooled
        {
            self.last_trigger = Some(now);
            self.base = Some(fresh);
            return true;
        }
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c(tx_bytes: u64, rx_bytes: u64, tx_packets: u64, rx_packets: u64) -> Counters {
        Counters {
            tx_bytes,
            rx_bytes,
            tx_packets,
            rx_packets,
        }
    }

    const LIMITS: Limits = Limits {
        bytes: 1000,
        records: 10,
    };

    #[test]
    fn either_direction_and_either_quantity_triggers() {
        let base = c(5000, 6000, 50, 60);
        assert!(!c(5999, 6999, 59, 69).exceeds(&base, &LIMITS));
        assert!(c(6000, 6000, 50, 60).exceeds(&base, &LIMITS), "tx bytes");
        assert!(c(5000, 7000, 50, 60).exceeds(&base, &LIMITS), "rx bytes");
        assert!(c(5000, 6000, 60, 60).exceeds(&base, &LIMITS), "tx records");
        assert!(c(5000, 6000, 50, 70).exceeds(&base, &LIMITS), "rx records");
    }

    #[test]
    fn a_counter_that_went_backwards_is_no_usage() {
        let base = c(5000, 5000, 50, 50);
        assert!(!c(0, 0, 0, 0).exceeds(&base, &LIMITS));
    }

    #[test]
    fn the_defaults_sit_below_the_kernel_soft_limits() {
        // vpt1_generation.h: soft 512 MiB and 524,288 records.
        const { assert!(REKEY_BYTES < 512 << 20 && REKEY_BYTES > 256 << 20) };
        const { assert!(REKEY_RECORDS < 524_288 && REKEY_RECORDS > 262_144) };
    }

    #[test]
    fn requests_once_per_generation_and_restarts_after_an_establishment() {
        let mut m = Monitor::new(LIMITS);
        assert!(
            !m.update(c(100, 0, 1, 0), 1),
            "the first reading is the baseline"
        );
        assert!(!m.update(c(1099, 0, 9, 0), 1));
        assert!(m.update(c(1100, 0, 9, 0), 1), "limit reached");
        assert!(
            !m.update(c(5000, 0, 99, 0), 1),
            "only once until the keys change"
        );
        // The exchange completed: the baseline restarts from the current counters.
        assert!(!m.update(c(5000, 0, 99, 0), 2));
        assert!(!m.update(c(5500, 0, 105, 0), 2));
        assert!(m.update(c(6000, 0, 105, 0), 2));
    }

    #[test]
    fn a_missing_device_reads_as_none() {
        assert!(Counters::read("vpt-no-such-device").is_none());
    }

    // ---- Silence ----

    fn at(base: Instant, secs: u64) -> Instant {
        base + Duration::from_secs(secs)
    }

    #[test]
    fn a_peer_that_keeps_answering_never_triggers() {
        let t0 = Instant::now();
        let mut s = Silence::new();
        assert!(!s.update(t0, c(0, 0, 0, 0), 1));
        for i in 1..=60u64 {
            assert!(
                !s.update(at(t0, i), c(0, 0, i * 10, i * 10), 1),
                "second {i}"
            );
        }
    }

    #[test]
    fn sending_with_no_answer_triggers_after_the_silence_time() {
        let t0 = Instant::now();
        let mut s = Silence::new();
        assert!(!s.update(t0, c(0, 0, 100, 100), 1));
        assert!(!s.update(at(t0, 5), c(0, 0, 150, 100), 1), "not yet");
        assert!(
            s.update(at(t0, 6), c(0, 0, 160, 100), 1),
            "6 s of silence while sending"
        );
    }

    #[test]
    fn an_idle_tunnel_or_a_few_stray_records_are_left_alone() {
        let t0 = Instant::now();
        let mut s = Silence::new();
        assert!(!s.update(t0, c(0, 0, 100, 100), 1));
        assert!(!s.update(at(t0, 30), c(0, 0, 100, 100), 1), "nothing sent");
        assert!(
            !s.update(at(t0, 60), c(0, 0, 104, 100), 1),
            "four records is below the minimum"
        );
    }

    #[test]
    fn it_waits_out_the_cooldown_before_triggering_again() {
        let t0 = Instant::now();
        let mut s = Silence::new();
        assert!(!s.update(t0, c(0, 0, 0, 0), 1));
        assert!(s.update(at(t0, 6), c(0, 0, 50, 0), 1));
        // Still silent: the window restarted at 6 s, so the next one is due at 12 s, but the
        // cooldown holds it until 21 s.
        assert!(!s.update(at(t0, 12), c(0, 0, 100, 0), 1));
        assert!(!s.update(at(t0, 20), c(0, 0, 150, 0), 1));
        assert!(s.update(at(t0, 21), c(0, 0, 160, 0), 1));
    }

    #[test]
    fn new_keys_or_a_recreated_device_start_the_count_again() {
        let t0 = Instant::now();
        let mut s = Silence::new();
        assert!(!s.update(t0, c(0, 0, 0, 0), 1));
        assert!(!s.update(at(t0, 5), c(0, 0, 50, 0), 1));
        // The re-key completed (count changed) one second before it would have triggered.
        assert!(!s.update(at(t0, 6), c(0, 0, 60, 0), 2));
        assert!(
            !s.update(at(t0, 11), c(0, 0, 100, 0), 2),
            "only 5 s since the new keys"
        );
        assert!(s.update(at(t0, 12), c(0, 0, 110, 0), 2));
        // The device was recreated: counters went backwards.
        assert!(!s.update(at(t0, 40), c(0, 0, 3, 0), 2));
        assert!(!s.update(at(t0, 45), c(0, 0, 20, 0), 2));
    }
}
