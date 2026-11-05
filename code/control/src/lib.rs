//! VPT1 userspace controller.
//!
//! The controller owns identity, configuration and the Noise IK exchange; the
//! kernel module owns every transport record (TECHNICAL_DESIGN.md section 3.1).

#[cfg(target_os = "linux")]
pub mod client;
pub mod clock;
pub mod config;
#[cfg(target_os = "linux")]
pub mod daemon;
pub mod exchange;
pub mod genl;
pub mod identity;
pub mod netlink_attrs;
pub mod netlink_ids;
pub mod network;
pub mod prefix;
pub mod session;
pub mod usage;
pub mod vptunnel;
pub mod wire;
