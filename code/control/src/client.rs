//! The `vptunnel` Generic Netlink client: one owner socket that claims a gateway, loads
//! its configuration, renews the lease and exchanges handshake messages with the kernel.
//!
//! The socket is the lease. Releasing it releases ownership, so it is opened once per
//! session and never closed to recover from an error (see `genl::socket`).

use std::time::Duration;

use thiserror::Error;

use crate::exchange::SessionKeys;
use crate::genl::inbox::{Event, TransportError};
use crate::genl::message::Request;
use crate::genl::socket::GenlSocket;
use crate::netlink_ids::*;
use crate::session::{Port, PortError};
use crate::vptunnel::{self, Status, VptunnelError};
use crate::wire::{ATTEMPT_ID_LEN, INCARNATION_LEN};

#[derive(Debug, Error)]
pub enum ClientError {
    #[error("netlink: {0}")]
    Transport(#[from] TransportError),
    #[error("{0}")]
    Vptunnel(#[from] VptunnelError),
    #[error("the kernel family `vptunnel` is not loaded: {0}")]
    NoFamily(TransportError),
    #[error("reply: {0}")]
    Reply(&'static str),
}

impl ClientError {
    pub fn errno(&self) -> Option<i32> {
        match self {
            ClientError::Transport(t) | ClientError::NoFamily(t) => t.errno(),
            _ => None,
        }
    }
}

impl From<ClientError> for PortError {
    fn from(e: ClientError) -> Self {
        PortError {
            errno: e.errno(),
            what: e.to_string(),
        }
    }
}

impl From<crate::genl::message::MessageError> for ClientError {
    fn from(e: crate::genl::message::MessageError) -> Self {
        ClientError::Vptunnel(VptunnelError::Message(e))
    }
}

pub struct Client {
    sock: GenlSocket,
    family: u16,
    ifindex: u32,
    controller: [u8; ATTR_CONTROLLER_ID_LEN],
}

impl Client {
    /// Open the owner socket and resolve the family. `request_budget` bounds one request.
    pub fn open(
        request_budget: Duration,
        ifindex: u32,
        controller: [u8; ATTR_CONTROLLER_ID_LEN],
    ) -> Result<Self, ClientError> {
        let mut sock = GenlSocket::open(request_budget)?;
        let family = sock.resolve_family(FAMILY).map_err(ClientError::NoFamily)?;
        Ok(Self {
            sock,
            family: family.id,
            ifindex,
            controller,
        })
    }

    fn req(&self, cmd: u8) -> Result<Request, ClientError> {
        Ok(vptunnel::request(
            self.family,
            cmd,
            self.ifindex,
            Some(&self.controller),
        )?)
    }

    pub fn claim(&mut self) -> Result<(), ClientError> {
        let r = self.req(CMD_CLAIM)?;
        self.sock.transact(r)?;
        Ok(())
    }

    pub fn renew(&mut self, revision: u64) -> Result<(), ClientError> {
        let mut r = self.req(CMD_RENEW)?;
        r.put_u64(ATTR_CONFIG_REVISION, revision)?;
        self.sock.transact(r)?;
        Ok(())
    }

    pub fn validate(&mut self, revision: u64, snapshot: &[u8]) -> Result<(), ClientError> {
        let mut r = self.req(CMD_VALIDATE)?;
        r.put_u64(ATTR_CONFIG_REVISION, revision)?;
        r.put_nested(ATTR_SNAPSHOT, snapshot)?;
        self.sock.transact(r)?;
        Ok(())
    }

    pub fn commit(&mut self, revision: u64) -> Result<(), ClientError> {
        let mut r = self.req(CMD_COMMIT_CONFIG)?;
        r.put_u64(ATTR_CONFIG_REVISION, revision)?;
        self.sock.transact(r)?;
        Ok(())
    }

    pub fn status(&mut self) -> Result<Status, ClientError> {
        // STATUS is not owner-gated: it needs only the interface.
        let r = vptunnel::request(self.family, CMD_STATUS, self.ifindex, None)?;
        let replies = self.sock.transact(r)?;
        match replies.as_slice() {
            [one] => Ok(vptunnel::parse_status(&one.payload)?),
            _ => Err(ClientError::Reply("expected exactly one STATUS reply")),
        }
    }

    /// The next kernel event, waiting up to `timeout`.
    pub fn recv_event(&mut self, timeout: Duration) -> Result<Option<Event>, ClientError> {
        Ok(self.sock.recv_event(timeout)?)
    }

    pub fn take_events_lost(&mut self) -> bool {
        self.sock.take_events_lost()
    }
}

impl Port for Client {
    fn exchange_tx(&mut self, slot: u8, bytes: &[u8]) -> Result<(), PortError> {
        let mut r = self.req(CMD_EXCHANGE_TX)?;
        r.put_u16(ATTR_PEER_SLOT, u16::from(slot))
            .map_err(ClientError::from)?;
        r.put_bytes(ATTR_EXCHANGE_BYTES, bytes)
            .map_err(ClientError::from)?;
        self.sock.transact(r).map_err(ClientError::from)?;
        Ok(())
    }

    fn stage(
        &mut self,
        slot: u8,
        kernel_incarnation: &[u8; INCARNATION_LEN],
        keys: &SessionKeys,
    ) -> Result<(), PortError> {
        let base = self.req(CMD_STAGE_GENERATION)?;
        let r = vptunnel::stage_generation(base, slot, kernel_incarnation, keys)
            .map_err(ClientError::from)?;
        self.sock.transact(r).map_err(ClientError::from)?;
        Ok(())
    }

    fn rekey_request(
        &mut self,
        slot: u8,
        kernel_incarnation: &[u8; INCARNATION_LEN],
        attempt: &[u8; ATTEMPT_ID_LEN],
    ) -> Result<(), PortError> {
        let base = self.req(CMD_REKEY_REQUEST)?;
        let r = vptunnel::rekey_request(base, slot, kernel_incarnation, attempt)
            .map_err(ClientError::from)?;
        self.sock.transact(r).map_err(ClientError::from)?;
        Ok(())
    }

    fn begin_attempt(
        &mut self,
        slot: u8,
        kernel_incarnation: &[u8; INCARNATION_LEN],
        given: Option<&[u8; ATTEMPT_ID_LEN]>,
    ) -> Result<([u8; ATTEMPT_ID_LEN], u64), PortError> {
        let base = self.req(CMD_BEGIN_ATTEMPT)?;
        let r = vptunnel::begin_attempt(base, slot, kernel_incarnation, given)
            .map_err(ClientError::from)?;
        let replies = self.sock.transact(r).map_err(ClientError::from)?;
        match replies.as_slice() {
            [one] => Ok(vptunnel::parse_attempt(&one.payload).map_err(ClientError::from)?),
            _ => Err(ClientError::Reply("expected exactly one BEGIN_ATTEMPT reply").into()),
        }
    }

    fn kernel_incarnation(&mut self, slot: u8) -> Result<[u8; INCARNATION_LEN], PortError> {
        let st = self.status()?;
        st.peers
            .iter()
            .find(|p| p.slot == slot)
            .map(|p| p.incarnation)
            .ok_or(PortError {
                what: format!("the kernel has no peer in slot {slot}"),
                errno: None,
            })
    }
}
