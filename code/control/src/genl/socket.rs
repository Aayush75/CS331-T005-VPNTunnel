//! Blocking Generic Netlink owner socket (Linux only).
//!
//! One socket per controller, one outstanding request at a time. Every
//! request carries a fresh nonzero sequence number and asks for an ACK. The
//! receive path (classification of replies, events, stale replies and
//! forgeries; bounds; total deadline) lives in [`super::inbox`] and is tested
//! without a kernel; this file is only the OS read behind it.
//!
//! The same socket carries kernel events to the owner, so unsolicited
//! messages are queued for [`GenlSocket::recv_event`] instead of discarded.
//! Datagrams not sent by the kernel (port 0) are never trusted.

use std::os::fd::OwnedFd;
use std::time::Duration;

use rustix::io::Errno;
use rustix::net::netlink::{self, SocketAddrNetlink};
use rustix::net::sockopt::{self, Timeout};
use rustix::net::{
    AddressFamily, RecvFlags, SendFlags, SocketFlags, SocketType, bind, getsockname, recvfrom,
    sendto, socket_with,
};

pub use super::inbox::{Event, OwnedReply, Stats, TransportError};
use super::inbox::{Inbox, Recv, Source, run_events, run_request};
use super::message::{
    FamilyInfo, MAX_MESSAGE_BYTES, NLM_F_ACK, Request, get_family_request, parse_family,
};

/// `SO_RCVTIMEO` of zero means "block forever", which is never wanted.
const MIN_READ_TIMEOUT: Duration = Duration::from_millis(1);

fn io_err(context: &'static str) -> impl FnOnce(Errno) -> TransportError {
    move |e| TransportError::Io {
        context,
        source: e.into(),
    }
}

/// The raw socket and its receive buffer: the real [`Source`].
struct Wire {
    fd: OwnedFd,
    buf: Vec<u8>,
}

impl Source for Wire {
    fn recv(&mut self, timeout: Duration) -> Result<Recv<'_>, TransportError> {
        sockopt::set_socket_timeout(&self.fd, Timeout::Recv, Some(timeout.max(MIN_READ_TIMEOUT)))
            .map_err(io_err("SO_RCVTIMEO"))?;
        match recvfrom(&self.fd, &mut self.buf[..], RecvFlags::TRUNC) {
            Ok((_, full_len, from)) => {
                if full_len > self.buf.len() {
                    return Err(TransportError::Truncated);
                }
                let from_kernel = from
                    .and_then(|a| SocketAddrNetlink::try_from(a).ok())
                    .is_some_and(|a| a.pid() == 0);
                Ok(Recv::Datagram {
                    bytes: &self.buf[..full_len],
                    from_kernel,
                })
            }
            Err(e) if e == Errno::AGAIN || e == Errno::WOULDBLOCK => Ok(Recv::TimedOut),
            Err(e) if e == Errno::INTR => Ok(Recv::Interrupted),
            // The kernel drops messages for a full socket and reports it once
            // on the next read; it is not a failure of the socket.
            Err(e) if e == Errno::NOBUFS => Ok(Recv::Overrun),
            Err(e) => Err(io_err("recvfrom")(e)),
        }
    }
}

pub struct GenlSocket {
    wire: Wire,
    inbox: Inbox,
    port: u32,
    seq: u32,
    budget: Duration,
}

impl GenlSocket {
    /// Open and bind a `NETLINK_GENERIC` socket; the kernel assigns its port.
    ///
    /// `request_budget` is the **total** time one [`transact`](Self::transact)
    /// may take, however many events arrive meanwhile.
    pub fn open(request_budget: Duration) -> Result<Self, TransportError> {
        let fd = socket_with(
            AddressFamily::NETLINK,
            SocketType::RAW,
            SocketFlags::CLOEXEC,
            Some(netlink::GENERIC),
        )
        .map_err(io_err("socket"))?;
        bind(&fd, &SocketAddrNetlink::new(0, 0)).map_err(io_err("bind"))?;
        let local = getsockname(&fd).map_err(io_err("getsockname"))?;
        let port = SocketAddrNetlink::try_from(local)
            .map_err(|_| TransportError::Unexpected("socket is not a Netlink socket"))?
            .pid();
        Ok(Self {
            wire: Wire {
                fd,
                buf: vec![0; MAX_MESSAGE_BYTES],
            },
            inbox: Inbox::new(port),
            port,
            seq: 0,
            budget: request_budget,
        })
    }

    /// The kernel-assigned Netlink port of this socket.
    pub fn port(&self) -> u32 {
        self.port
    }

    fn next_seq(&mut self) -> u32 {
        // Zero is reserved for kernel events, so requests never use it.
        self.seq = self.seq.wrapping_add(1).max(1);
        self.seq
    }

    /// Send `req` and collect its data replies until the kernel's ACK.
    ///
    /// The request is always sent with `NLM_F_ACK`, so success is explicit:
    /// an error reply becomes [`TransportError::Kernel`], and a missing ACK
    /// becomes [`TransportError::Timeout`]. Replies to abandoned requests
    /// are dropped; unsolicited events are queued, not discarded.
    pub fn transact(&mut self, req: Request) -> Result<Vec<OwnedReply>, TransportError> {
        let seq = self.next_seq();
        let bytes = req.finish(seq);
        let flags = u16::from_ne_bytes([bytes[6], bytes[7]]);
        if flags & NLM_F_ACK == 0 {
            return Err(TransportError::Unexpected(
                "request does not ask for an ACK",
            ));
        }
        let sent = sendto(
            &self.wire.fd,
            &bytes,
            SendFlags::empty(),
            &SocketAddrNetlink::new(0, 0),
        )
        .map_err(io_err("sendto"))?;
        if sent != bytes.len() {
            return Err(TransportError::Unexpected("short Netlink send"));
        }
        drop(bytes);
        run_request(&mut self.wire, &mut self.inbox, seq, self.budget)
    }

    /// The next kernel event for this owner, waiting up to `timeout`.
    ///
    /// `Ok(None)` means nothing arrived or the kernel reported `ENOBUFS` (the
    /// wait ends early); in both cases call
    /// [`take_events_lost`](Self::take_events_lost).
    pub fn recv_event(&mut self, timeout: Duration) -> Result<Option<Event>, TransportError> {
        run_events(&mut self.wire, &mut self.inbox, timeout)
    }

    /// True once if events may have been lost (kernel `ENOBUFS` or the local
    /// queue overflowed) since the last call. The controller must then resync
    /// from `STATUS`; it must never close and reopen this socket to recover,
    /// because releasing it releases the lease.
    pub fn take_events_lost(&mut self) -> bool {
        self.inbox.take_events_lost()
    }

    /// Events received and queued but not yet taken.
    pub fn pending_events(&self) -> usize {
        self.inbox.pending_events()
    }

    pub fn stats(&self) -> Stats {
        self.inbox.stats()
    }

    /// Resolve a Generic Netlink family by name through `nlctrl`.
    pub fn resolve_family(&mut self, name: &str) -> Result<FamilyInfo, TransportError> {
        let replies = self.transact(get_family_request(name)?)?;
        match replies.as_slice() {
            [one] if one.kind == super::message::GENL_ID_CTRL => {
                Ok(parse_family(&one.payload, name)?)
            }
            [_] => Err(TransportError::Unexpected(
                "family reply from another family",
            )),
            _ => Err(TransportError::Unexpected(
                "expected exactly one family reply",
            )),
        }
    }
}
