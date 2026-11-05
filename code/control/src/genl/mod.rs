//! Generic Netlink transport for the controller (RJ3).
//!
//! `message` (framing) and `inbox` (the receive path: replies, events, stale
//! replies, bounds, deadline) are portable and tested on every platform.
//! `socket` is the thin OS layer and exists only on Linux; macOS results never
//! qualify it (CODING_STANDARDS.md section 7).

pub mod inbox;
pub mod message;
#[cfg(target_os = "linux")]
pub mod socket;
