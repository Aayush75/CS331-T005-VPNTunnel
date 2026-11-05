//! Suspend-aware time for the controller's deadlines.
//!
//! `std::time::Instant` is CLOCK_MONOTONIC on Linux, which stops while the machine is suspended:
//! after a suspend the lease renewal, rekey and retry deadlines would all be late by the time
//! asleep. `Instant` cannot be built from another clock, so `BootClock` keeps an `Instant`
//! anchor and adds the CLOCK_BOOTTIME time elapsed since it was taken. The `Controller` keeps
//! taking `Instant` values, so its code and tests are unchanged. Off Linux, or if the clock
//! cannot be read, `now` is plain `Instant::now()`.

use std::cell::Cell;
use std::time::{Duration, Instant};

/// Raw CLOCK_BOOTTIME reading, `None` when it cannot be read. A function pointer so tests can
/// substitute a clock they control.
type BootSource = fn() -> Option<Duration>;

#[derive(Debug, Clone)]
pub struct BootClock {
    anchor: Instant,
    boot_at_anchor: Option<Duration>,
    source: BootSource,
    /// Largest value `now` has returned, so a failed read never makes time step backwards.
    last: Cell<Instant>,
}

#[cfg(target_os = "linux")]
fn boottime() -> Option<Duration> {
    let t = rustix::time::clock_gettime(rustix::time::ClockId::Boottime);
    Some(Duration::new(
        u64::try_from(t.tv_sec).ok()?,
        u32::try_from(t.tv_nsec).ok()?,
    ))
}

#[cfg(not(target_os = "linux"))]
fn boottime() -> Option<Duration> {
    None
}

impl BootClock {
    pub fn new() -> Self {
        Self::with_source(boottime)
    }

    fn with_source(source: BootSource) -> Self {
        let anchor = Instant::now();
        BootClock {
            anchor,
            boot_at_anchor: source(),
            source,
            last: Cell::new(anchor),
        }
    }

    /// Never returns less than an earlier call: if the boot-time read fails, the fallback
    /// `Instant::now()` can trail the boot-time value by the time spent suspended, so the larger
    /// of the two is kept.
    pub fn now(&self) -> Instant {
        let candidate = match (self.boot_at_anchor, (self.source)()) {
            (Some(at_anchor), Some(now)) => self.anchor + now.saturating_sub(at_anchor),
            _ => Instant::now(),
        };
        let v = candidate.max(self.last.get());
        self.last.set(v);
        v
    }
}

impl Default for BootClock {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    #[test]
    fn never_runs_backwards_and_never_trails_the_monotonic_clock_by_much() {
        let c = BootClock::new();
        let a = c.now();
        std::thread::sleep(Duration::from_millis(20));
        let b = c.now();
        assert!(b >= a);
        assert!(b.duration_since(a) >= Duration::from_millis(20));
        // Without a suspend the two clocks agree to well within a second.
        let skew = if b > Instant::now() {
            b - Instant::now()
        } else {
            Instant::now() - b
        };
        assert!(skew < Duration::from_secs(1));
    }

    // Fake boot-time sources. Each test uses its own static so tests can run in parallel.
    // u64::MAX means "the read fails".
    static SUSPEND_NS: AtomicU64 = AtomicU64::new(1_000_000_000);
    fn suspend_source() -> Option<Duration> {
        Some(Duration::from_nanos(SUSPEND_NS.load(Ordering::SeqCst)))
    }

    #[test]
    fn a_suspend_sized_jump_makes_lease_renewal_and_rekey_deadlines_fire() {
        let lease_interval = Duration::from_secs(60);
        let rekey_after = Duration::from_secs(600);
        let c = BootClock::with_source(suspend_source);
        let t0 = c.now();
        let renew_due = t0 + lease_interval;
        let rekey_due = t0 + rekey_after;
        // Not due yet just after start.
        assert!(c.now() < renew_due && c.now() < rekey_due);
        // Machine suspends for an hour: boot time jumps, a monotonic reading would not.
        SUSPEND_NS.fetch_add(3_600 * 1_000_000_000, Ordering::SeqCst);
        let after = c.now();
        assert!(
            after >= renew_due,
            "lease renewal must be due after the suspend"
        );
        assert!(after >= rekey_due, "re-key must be due after the suspend");
        assert!(after.duration_since(t0) >= Duration::from_secs(3_600));
    }

    static FAIL_NS: AtomicU64 = AtomicU64::new(1_000_000_000);
    fn failing_source() -> Option<Duration> {
        match FAIL_NS.load(Ordering::SeqCst) {
            u64::MAX => None,
            ns => Some(Duration::from_nanos(ns)),
        }
    }

    #[test]
    fn a_failed_boot_time_read_does_not_step_time_backwards() {
        let c = BootClock::with_source(failing_source);
        // Suspend for an hour, observe it, then the read starts failing: the fallback
        // Instant::now() is an hour behind, but now() must hold the larger value.
        FAIL_NS.fetch_add(3_600 * 1_000_000_000, Ordering::SeqCst);
        let high = c.now();
        FAIL_NS.store(u64::MAX, Ordering::SeqCst);
        let after_failure = c.now();
        assert!(after_failure >= high);
        let again = c.now();
        assert!(again >= after_failure);
    }
}
