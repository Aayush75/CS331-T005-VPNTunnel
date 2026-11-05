//! Generated from `protocol/netlink-v1.json`; do not edit.
//! Identifiers only: this module does not validate or send Netlink messages.

pub const FAMILY: &str = "vptunnel";
pub const VERSION: u8 = 1;
pub const MAX_MESSAGE_BYTES: usize = 65536;

pub const CMD_CLAIM: u8 = 1;
pub const CMD_RENEW: u8 = 2;
pub const CMD_VALIDATE: u8 = 3;
pub const CMD_COMMIT_CONFIG: u8 = 4;
pub const CMD_STAGE_GENERATION: u8 = 5;
pub const CMD_EXCHANGE_TX: u8 = 6;
pub const CMD_CONTROL_INTENT: u8 = 7;
pub const CMD_DISABLE: u8 = 8;
pub const CMD_REVOKE: u8 = 9;
pub const CMD_STATUS: u8 = 10;
pub const CMD_COUNTERS: u8 = 11;
pub const CMD_REMOVE_GATEWAY: u8 = 12;
pub const CMD_BEGIN_ATTEMPT: u8 = 13;
pub const CMD_REKEY_REQUEST: u8 = 14;
pub const CMD_EXCHANGE_RX: u8 = 128;
pub const CMD_REKEY_GRANTED: u8 = 129;
pub const CMD_REKEY_NEEDED: u8 = 130;

pub const ATTR_IFINDEX: u16 = 1;
pub const ATTR_CONTROLLER_ID: u16 = 2;
pub const ATTR_CONTROLLER_ID_LEN: usize = 16;
pub const ATTR_CONFIG_REVISION: u16 = 3;
pub const ATTR_PEER_SLOT: u16 = 4;
pub const ATTR_PEER_INCARNATION: u16 = 5;
pub const ATTR_PEER_INCARNATION_LEN: usize = 16;
pub const ATTR_ATTEMPT_ID: u16 = 6;
pub const ATTR_ATTEMPT_ID_LEN: usize = 16;
pub const ATTR_HANDSHAKE_HASH: u16 = 7;
pub const ATTR_HANDSHAKE_HASH_LEN: usize = 32;
pub const ATTR_LOCAL_RX: u16 = 8;
pub const ATTR_REMOTE_RX: u16 = 9;
pub const ATTR_TX_KEY: u16 = 10;
pub const ATTR_TX_KEY_LEN: usize = 32;
pub const ATTR_RX_KEY: u16 = 11;
pub const ATTR_RX_KEY_LEN: usize = 32;
pub const ATTR_MODE: u16 = 12;
pub const ATTR_PARENT_HASH: u16 = 13;
pub const ATTR_PARENT_HASH_LEN: usize = 32;
pub const ATTR_EXCHANGE_BYTES: u16 = 14;
pub const ATTR_EXCHANGE_BYTES_MAX_LEN: usize = 224;
pub const ATTR_SNAPSHOT: u16 = 15;
pub const ATTR_SNAPSHOT_MAX_LEN: usize = 61440;
pub const ATTR_INTENT: u16 = 16;
pub const ATTR_STATUS: u16 = 17;
pub const ATTR_COUNTERS: u16 = 18;
pub const ATTR_REQUEST_ID: u16 = 19;

/// True for attributes that must carry `NLA_F_NESTED` (and only those).
pub const fn attr_is_nested(id: u16) -> bool {
    matches!(id, ATTR_SNAPSHOT | ATTR_STATUS | ATTR_COUNTERS)
}
