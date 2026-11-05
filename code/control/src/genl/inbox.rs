//! Portable receive path for a Generic Netlink owner socket.
//!
//! The owner socket carries two kinds of kernel traffic at once: replies to
//! the controller's own requests, and unsolicited events (`EXCHANGE_RX`, a
//! unicast to the owner's port; docs/AY2_CONTRACT.md section 2). Ownership is
//! bound to the portid, so both must share one socket, and an event must never
//! be thrown away just because a reply is awaited.
//!
//! Classification, by `nlmsg_seq`:
//!
//! * `0` and a family message type: an **event**, queued (bounded).
//! * The awaited sequence number: the **reply** being waited for.
//! * Any other nonzero sequence: a **stale reply** to an abandoned request,
//!   dropped and counted. This controller's own sequence numbers start at 1.
//!
//! Datagrams not sent by the kernel (port 0) are never trusted. Overflowing
//! the bounded queue or a kernel `ENOBUFS` means events were lost; that is
//! reported through [`Inbox::take_events_lost`] so the controller resyncs
//! instead of assuming it saw everything. The OS read is behind [`Source`],
//! so the whole receive path is tested without a kernel.

use std::collections::VecDeque;
use std::io;
use std::time::{Duration, Instant};

use thiserror::Error;

use super::message::{
    MAX_MESSAGE_BYTES, MessageError, NLM_F_MULTI, NLMSG_MIN_TYPE, Reply, classify, split_messages,
};

/// Unsolicited events kept before newer ones are dropped and counted.
pub const MAX_PENDING_EVENTS: usize = 64;
/// Total payload bytes of queued events.
pub const MAX_PENDING_EVENT_BYTES: usize = 256 * 1024;
/// Data messages kept for one request (no dumps are used).
pub const MAX_REPLIES_PER_REQUEST: usize = 8;

#[derive(Debug, Error)]
pub enum TransportError {
    #[error("{context}: {source}")]
    Io {
        context: &'static str,
        #[source]
        source: io::Error,
    },
    #[error("no reply within the receive timeout")]
    Timeout,
    #[error("datagram larger than {MAX_MESSAGE_BYTES} bytes")]
    Truncated,
    #[error("malformed reply: {0}")]
    Message(#[from] MessageError),
    #[error("kernel rejected the request: {}", io::Error::from_raw_os_error(*.errno))]
    Kernel { errno: i32 },
    #[error("unexpected reply: {0}")]
    Unexpected(&'static str),
}

impl TransportError {
    /// The kernel's errno, if the kernel answered with an error.
    pub fn errno(&self) -> Option<i32> {
        match self {
            TransportError::Kernel { errno } => Some(*errno),
            _ => None,
        }
    }
}

/// An owned copy of one data reply (type >= 16) for the outstanding request.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OwnedReply {
    pub kind: u16,
    pub flags: u16,
    /// Generic Netlink header and attributes.
    pub payload: Vec<u8>,
}

/// An unsolicited kernel message (`nlmsg_seq == 0`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Event {
    /// The message type, which for a Generic Netlink family is its id.
    pub family: u16,
    /// Generic Netlink header and attributes.
    pub payload: Vec<u8>,
}

/// Counters for diagnostics and tests; never reset.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Stats {
    pub events_queued: u64,
    /// Dropped because the queue bound was reached.
    pub events_overflowed: u64,
    /// Kernel `ENOBUFS` notifications (events were dropped in the kernel).
    pub overruns: u64,
    pub stale_replies: u64,
    /// Datagrams whose sender was not the kernel.
    pub foreign_datagrams: u64,
    /// Netlink control messages with sequence 0 (never expected).
    pub ignored_control: u64,
}

/// What one blocking read produced.
pub enum Recv<'a> {
    Datagram {
        bytes: &'a [u8],
        from_kernel: bool,
    },
    TimedOut,
    Interrupted,
    /// The kernel flagged `ENOBUFS`: it dropped messages for this socket.
    Overrun,
}

/// The OS read, so the receive path can be driven by a script in tests.
pub trait Source {
    /// Wait up to `timeout` (never zero) for one datagram.
    fn recv(&mut self, timeout: Duration) -> Result<Recv<'_>, TransportError>;
}

#[derive(Debug, PartialEq, Eq)]
pub enum Flow {
    Pending,
    Complete,
}

pub struct Inbox {
    port: u32,
    events: VecDeque<Event>,
    queued_bytes: usize,
    lost: bool,
    stats: Stats,
}

impl Inbox {
    pub fn new(port: u32) -> Self {
        Self {
            port,
            events: VecDeque::new(),
            queued_bytes: 0,
            lost: false,
            stats: Stats::default(),
        }
    }

    pub fn stats(&self) -> Stats {
        self.stats
    }

    pub fn pending_events(&self) -> usize {
        self.events.len()
    }

    /// Oldest queued event.
    pub fn take_event(&mut self) -> Option<Event> {
        let event = self.events.pop_front()?;
        self.queued_bytes -= event.payload.len();
        Some(event)
    }

    /// True once if events may have been lost (kernel `ENOBUFS` or queue
    /// overflow) since the last call. The controller must then resync from
    /// `STATUS` instead of assuming it saw every event.
    pub fn take_events_lost(&mut self) -> bool {
        std::mem::take(&mut self.lost)
    }

    fn mark_overrun(&mut self) {
        self.stats.overruns += 1;
        self.lost = true;
    }

    fn push_event(&mut self, family: u16, payload: &[u8]) {
        if self.events.len() >= MAX_PENDING_EVENTS
            || self.queued_bytes + payload.len() > MAX_PENDING_EVENT_BYTES
        {
            self.stats.events_overflowed += 1;
            self.lost = true;
            return;
        }
        self.queued_bytes += payload.len();
        self.stats.events_queued += 1;
        self.events.push_back(Event {
            family,
            payload: payload.to_vec(),
        });
    }

    /// Classify every message of one kernel datagram.
    ///
    /// The whole datagram is always consumed, so an event that shares it with
    /// the awaited ACK is kept. The first terminal result for the awaited
    /// sequence wins; later messages never override it.
    pub fn process(
        &mut self,
        data: &[u8],
        awaiting: Option<u32>,
        replies: &mut Vec<OwnedReply>,
    ) -> Result<Flow, TransportError> {
        let mut terminal: Option<Result<Flow, TransportError>> = None;
        for msg in split_messages(data)? {
            let seq = msg.header.seq;
            if seq == 0 {
                if msg.header.kind >= NLMSG_MIN_TYPE {
                    self.push_event(msg.header.kind, msg.payload);
                } else {
                    self.stats.ignored_control += 1;
                }
                continue;
            }
            if awaiting != Some(seq) || terminal.is_some() {
                self.stats.stale_replies += 1;
                continue;
            }
            if msg.header.pid != self.port {
                terminal = Some(Err(TransportError::Unexpected(
                    "reply addressed to another port",
                )));
                continue;
            }
            match classify(&msg) {
                Err(e) => terminal = Some(Err(e.into())),
                Ok(Reply::Ack { seq: s }) if s == seq => terminal = Some(Ok(Flow::Complete)),
                Ok(Reply::Error { seq: s, errno }) if s == seq => {
                    terminal = Some(Err(TransportError::Kernel { errno }));
                }
                Ok(Reply::Ack { .. } | Reply::Error { .. }) => {
                    terminal = Some(Err(TransportError::Unexpected("ACK names another request")));
                }
                Ok(Reply::Noop) => {}
                Ok(Reply::Done) => {
                    terminal = Some(Err(TransportError::Unexpected("DONE without a dump")));
                }
                Ok(Reply::Data(m)) => {
                    if m.header.flags & NLM_F_MULTI != 0 {
                        terminal = Some(Err(TransportError::Unexpected("multipart reply")));
                    } else if replies.len() == MAX_REPLIES_PER_REQUEST {
                        terminal = Some(Err(TransportError::Unexpected("too many replies")));
                    } else {
                        replies.push(OwnedReply {
                            kind: m.header.kind,
                            flags: m.header.flags,
                            payload: m.payload.to_vec(),
                        });
                    }
                }
            }
        }
        terminal.unwrap_or(Ok(Flow::Pending))
    }
}

/// Read until the kernel's answer for `seq`, within one total `budget`.
///
/// The budget covers the whole request however many events arrive meanwhile,
/// so an event flood cannot stretch a lease renewal past its deadline.
/// Events seen on the way are queued, never discarded.
pub fn run_request<S: Source>(
    src: &mut S,
    inbox: &mut Inbox,
    seq: u32,
    budget: Duration,
) -> Result<Vec<OwnedReply>, TransportError> {
    let deadline = Instant::now() + budget;
    let mut replies = Vec::new();
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(TransportError::Timeout);
        }
        match src.recv(remaining)? {
            Recv::TimedOut => return Err(TransportError::Timeout),
            Recv::Interrupted => {}
            Recv::Overrun => inbox.mark_overrun(),
            Recv::Datagram { bytes, from_kernel } => {
                if !from_kernel {
                    inbox.stats.foreign_datagrams += 1;
                } else if inbox.process(bytes, Some(seq), &mut replies)? == Flow::Complete {
                    return Ok(replies);
                }
            }
        }
    }
}

/// Next event: a queued one at once, otherwise wait up to `budget`.
///
/// `Ok(None)` means nothing arrived, or the kernel reported `ENOBUFS` (the
/// read is woken early); check [`Inbox::take_events_lost`] either way.
pub fn run_events<S: Source>(
    src: &mut S,
    inbox: &mut Inbox,
    budget: Duration,
) -> Result<Option<Event>, TransportError> {
    if let Some(event) = inbox.take_event() {
        return Ok(Some(event));
    }
    let deadline = Instant::now() + budget;
    let mut ignored = Vec::new();
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Ok(None);
        }
        match src.recv(remaining)? {
            Recv::TimedOut => return Ok(None),
            Recv::Interrupted => {}
            Recv::Overrun => {
                inbox.mark_overrun();
                return Ok(None);
            }
            Recv::Datagram { bytes, from_kernel } => {
                if !from_kernel {
                    inbox.stats.foreign_datagrams += 1;
                    continue;
                }
                inbox.process(bytes, None, &mut ignored)?;
                if let Some(event) = inbox.take_event() {
                    return Ok(Some(event));
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::genl::message::{NLM_F_ACK, NLM_F_REQUEST, NLMSG_DONE, NLMSG_ERROR, NLMSG_NOOP};

    const PORT: u32 = 4242;
    const FAMILY: u16 = 0x30;
    const BUDGET: Duration = Duration::from_millis(200);

    fn msg(kind: u16, flags: u16, seq: u32, pid: u32, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&((16 + payload.len()) as u32).to_ne_bytes());
        out.extend_from_slice(&kind.to_ne_bytes());
        out.extend_from_slice(&flags.to_ne_bytes());
        out.extend_from_slice(&seq.to_ne_bytes());
        out.extend_from_slice(&pid.to_ne_bytes());
        out.extend_from_slice(payload);
        out.resize(out.len().next_multiple_of(4), 0);
        out
    }

    fn event(payload: &[u8]) -> Vec<u8> {
        msg(FAMILY, 0, 0, 0, payload)
    }

    fn data(seq: u32, payload: &[u8]) -> Vec<u8> {
        msg(FAMILY, 0, seq, PORT, payload)
    }

    fn status(code: i32, seq: u32) -> Vec<u8> {
        let original = msg(FAMILY, NLM_F_REQUEST | NLM_F_ACK, seq, 0, &[]);
        let mut p = code.to_ne_bytes().to_vec();
        p.extend_from_slice(&original);
        msg(NLMSG_ERROR, 0, seq, PORT, &p)
    }

    fn ack(seq: u32) -> Vec<u8> {
        status(0, seq)
    }

    fn concat(parts: &[Vec<u8>]) -> Vec<u8> {
        parts.concat()
    }

    enum Step {
        Kernel(Vec<u8>),
        Foreign(Vec<u8>),
        Timeout,
        Intr,
        Overrun,
    }

    struct Script {
        steps: VecDeque<Step>,
        current: Vec<u8>,
    }

    impl Script {
        fn new(steps: Vec<Step>) -> Self {
            Self {
                steps: steps.into(),
                current: Vec::new(),
            }
        }
    }

    impl Source for Script {
        fn recv(&mut self, _timeout: Duration) -> Result<Recv<'_>, TransportError> {
            let from_kernel = match self.steps.pop_front() {
                None | Some(Step::Timeout) => return Ok(Recv::TimedOut),
                Some(Step::Intr) => return Ok(Recv::Interrupted),
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

    fn source(steps: Vec<Step>) -> Script {
        Script::new(steps)
    }

    fn drain(inbox: &mut Inbox) -> Vec<Vec<u8>> {
        std::iter::from_fn(|| inbox.take_event())
            .map(|e| e.payload)
            .collect()
    }

    #[test]
    fn event_before_the_ack_is_kept() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![Step::Kernel(event(b"INIT-1")), Step::Kernel(ack(7))]);
        let replies = run_request(&mut src, &mut inbox, 7, BUDGET).unwrap();
        assert!(replies.is_empty());
        assert_eq!(drain(&mut inbox), vec![b"INIT-1".to_vec()]);
        assert!(!inbox.take_events_lost());
    }

    #[test]
    fn event_sharing_a_datagram_with_the_ack_is_kept() {
        let mut inbox = Inbox::new(PORT);
        let both = concat(&[ack(7), event(b"AFTERACK")]);
        let mut src = source(vec![Step::Kernel(both)]);
        run_request(&mut src, &mut inbox, 7, BUDGET).unwrap();
        assert_eq!(drain(&mut inbox), vec![b"AFTERACK".to_vec()]);
    }

    #[test]
    fn events_interleaved_with_data_replies_keep_order() {
        let mut inbox = Inbox::new(PORT);
        let d = concat(&[
            event(b"E1\0\0"),
            data(9, b"REPLY\0\0\0"),
            event(b"E2\0\0"),
            ack(9),
            event(b"E3\0\0"),
        ]);
        let mut src = source(vec![Step::Kernel(d)]);
        let replies = run_request(&mut src, &mut inbox, 9, BUDGET).unwrap();
        assert_eq!(replies.len(), 1);
        assert_eq!(replies[0].payload, b"REPLY\0\0\0");
        assert_eq!(
            drain(&mut inbox),
            vec![b"E1\0\0".to_vec(), b"E2\0\0".to_vec(), b"E3\0\0".to_vec()]
        );
    }

    #[test]
    fn stale_replies_are_dropped_and_counted_not_queued() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![
            Step::Kernel(ack(3)),
            Step::Kernel(data(2, b"OLDOLDOL")),
            Step::Kernel(ack(5)),
        ]);
        run_request(&mut src, &mut inbox, 5, BUDGET).unwrap();
        assert_eq!(inbox.pending_events(), 0);
        assert_eq!(inbox.stats().stale_replies, 2);
    }

    #[test]
    fn kernel_error_is_returned_and_events_survive() {
        let mut inbox = Inbox::new(PORT);
        let both = concat(&[event(b"KEEPME\0\0"), status(-1, 4)]);
        let mut src = source(vec![Step::Kernel(both)]);
        let err = run_request(&mut src, &mut inbox, 4, BUDGET).unwrap_err();
        assert_eq!(err.errno(), Some(1));
        assert_eq!(inbox.pending_events(), 1);
    }

    #[test]
    fn first_terminal_result_wins_within_a_datagram() {
        let mut inbox = Inbox::new(PORT);
        let both = concat(&[status(-13, 4), ack(4)]);
        let mut src = source(vec![Step::Kernel(both)]);
        let err = run_request(&mut src, &mut inbox, 4, BUDGET).unwrap_err();
        assert_eq!(err.errno(), Some(13));
    }

    #[test]
    fn flood_is_bounded_by_count_and_reported_lost() {
        let mut inbox = Inbox::new(PORT);
        let mut steps: Vec<Step> = (0..200u32)
            .map(|i| Step::Kernel(event(&i.to_ne_bytes())))
            .collect();
        steps.push(Step::Kernel(ack(1)));
        let mut src = source(steps);
        run_request(&mut src, &mut inbox, 1, Duration::from_secs(5)).unwrap();
        assert_eq!(inbox.pending_events(), MAX_PENDING_EVENTS);
        assert_eq!(inbox.stats().events_queued, MAX_PENDING_EVENTS as u64);
        assert_eq!(
            inbox.stats().events_overflowed,
            200 - MAX_PENDING_EVENTS as u64
        );
        assert!(inbox.take_events_lost());
        assert!(!inbox.take_events_lost(), "the flag is taken once");
        // Oldest first: the survivors are the first 64.
        let first = inbox.take_event().unwrap().payload;
        assert_eq!(first, 0u32.to_ne_bytes());
    }

    #[test]
    fn flood_is_bounded_by_bytes() {
        let mut inbox = Inbox::new(PORT);
        let big = vec![7u8; 40 * 1024];
        let mut steps: Vec<Step> = (0..10).map(|_| Step::Kernel(event(&big))).collect();
        steps.push(Step::Kernel(ack(1)));
        let mut src = source(steps);
        run_request(&mut src, &mut inbox, 1, Duration::from_secs(5)).unwrap();
        assert_eq!(inbox.pending_events(), MAX_PENDING_EVENT_BYTES / big.len());
        assert!(inbox.take_events_lost());
        // Draining frees the byte budget again.
        while inbox.take_event().is_some() {}
        let mut src = source(vec![Step::Kernel(event(&big)), Step::Kernel(ack(2))]);
        run_request(&mut src, &mut inbox, 2, BUDGET).unwrap();
        assert_eq!(inbox.pending_events(), 1);
        assert!(!inbox.take_events_lost());
    }

    #[test]
    fn enobufs_marks_lost_and_the_request_still_completes() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![
            Step::Overrun,
            Step::Kernel(event(b"AFTERENO")),
            Step::Kernel(ack(6)),
        ]);
        run_request(&mut src, &mut inbox, 6, BUDGET).unwrap();
        assert!(inbox.take_events_lost());
        assert_eq!(inbox.stats().overruns, 1);
        assert_eq!(inbox.pending_events(), 1);
    }

    #[test]
    fn timeout_is_an_error_and_keeps_queued_events() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![Step::Kernel(event(b"STILLHER")), Step::Timeout]);
        let err = run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err();
        assert!(matches!(err, TransportError::Timeout));
        assert_eq!(inbox.pending_events(), 1);
    }

    #[test]
    fn interrupted_reads_are_retried() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![Step::Intr, Step::Intr, Step::Kernel(ack(1))]);
        run_request(&mut src, &mut inbox, 1, BUDGET).unwrap();
    }

    #[test]
    fn non_kernel_datagrams_are_ignored_even_if_they_look_right() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![
            Step::Foreign(ack(1)),
            Step::Foreign(event(b"FORGED\0\0")),
            Step::Foreign(status(-1, 1)),
            Step::Kernel(ack(1)),
        ]);
        // The forged ACK, event and error are all skipped; only the kernel's ACK counts.
        run_request(&mut src, &mut inbox, 1, BUDGET).unwrap();
        assert_eq!(inbox.pending_events(), 0);
        assert_eq!(inbox.stats().foreign_datagrams, 3);
    }

    #[test]
    fn forged_datagrams_cannot_complete_a_request() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![Step::Foreign(ack(1)), Step::Timeout]);
        let err = run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err();
        assert!(matches!(err, TransportError::Timeout));
    }

    #[test]
    fn reply_for_another_port_is_an_error() {
        let mut inbox = Inbox::new(PORT);
        let wrong = msg(FAMILY, 0, 1, PORT + 1, b"WRONGPRT");
        let mut src = source(vec![Step::Kernel(wrong)]);
        let err = run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err();
        assert!(matches!(err, TransportError::Unexpected(m) if m.contains("another port")));
    }

    #[test]
    fn sequence_zero_control_messages_are_ignored_and_counted() {
        let mut inbox = Inbox::new(PORT);
        let d = concat(&[
            msg(NLMSG_ERROR, 0, 0, 0, &[0; 20]),
            msg(NLMSG_DONE, 0, 0, 0, &[0; 4]),
            msg(NLMSG_NOOP, 0, 0, 0, &[]),
            ack(1),
        ]);
        let mut src = source(vec![Step::Kernel(d)]);
        run_request(&mut src, &mut inbox, 1, BUDGET).unwrap();
        assert_eq!(inbox.stats().ignored_control, 3);
        assert_eq!(inbox.pending_events(), 0);
    }

    #[test]
    fn multipart_too_many_replies_and_done_are_refused() {
        let mut inbox = Inbox::new(PORT);
        let multi = msg(FAMILY, NLM_F_MULTI, 1, PORT, b"MULTIPRT");
        let mut src = source(vec![Step::Kernel(multi)]);
        assert!(matches!(
            run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err(),
            TransportError::Unexpected(m) if m.contains("multipart")
        ));

        let many: Vec<Vec<u8>> = (0..=MAX_REPLIES_PER_REQUEST)
            .map(|_| data(1, b"ABCDEFGH"))
            .collect();
        let mut src = source(vec![Step::Kernel(concat(&many))]);
        assert!(matches!(
            run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err(),
            TransportError::Unexpected(m) if m.contains("too many")
        ));

        let done = msg(NLMSG_DONE, 0, 1, PORT, &[0; 4]);
        let mut src = source(vec![Step::Kernel(done)]);
        assert!(matches!(
            run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err(),
            TransportError::Unexpected(m) if m.contains("DONE")
        ));
    }

    #[test]
    fn malformed_kernel_datagram_is_an_error() {
        let mut inbox = Inbox::new(PORT);
        let mut bad = ack(1);
        bad[0..4].copy_from_slice(&9u32.to_ne_bytes());
        let mut src = source(vec![Step::Kernel(bad)]);
        assert!(matches!(
            run_request(&mut src, &mut inbox, 1, BUDGET).unwrap_err(),
            TransportError::Message(_)
        ));
    }

    /// A source that never stops delivering events, slowly.
    struct Flood {
        current: Vec<u8>,
        delay: Duration,
        reads: usize,
    }

    impl Source for Flood {
        fn recv(&mut self, _timeout: Duration) -> Result<Recv<'_>, TransportError> {
            std::thread::sleep(self.delay);
            self.reads += 1;
            self.current = event(b"FLOODFLD");
            Ok(Recv::Datagram {
                bytes: &self.current,
                from_kernel: true,
            })
        }
    }

    #[test]
    fn total_deadline_holds_under_an_endless_event_flood() {
        let mut inbox = Inbox::new(PORT);
        let mut src = Flood {
            current: Vec::new(),
            delay: Duration::from_millis(5),
            reads: 0,
        };
        let budget = Duration::from_millis(150);
        let start = Instant::now();
        let err = run_request(&mut src, &mut inbox, 1, budget).unwrap_err();
        let took = start.elapsed();
        assert!(matches!(err, TransportError::Timeout));
        assert!(took >= budget, "returned early: {took:?}");
        assert!(
            took < budget * 4,
            "the flood stretched the deadline: {took:?}"
        );
        assert!(src.reads > 5, "the flood really was being read");
        // Every event read before the deadline was kept (well under the bound).
        assert!(src.reads < MAX_PENDING_EVENTS);
        assert_eq!(inbox.pending_events(), src.reads);
    }

    #[test]
    fn run_events_returns_queued_events_first_in_order() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![
            Step::Kernel(concat(&[event(b"ONE\0"), event(b"TWO\0")])),
            Step::Kernel(ack(1)),
        ]);
        run_request(&mut src, &mut inbox, 1, BUDGET).unwrap();
        let mut idle = source(vec![]);
        let a = run_events(&mut idle, &mut inbox, BUDGET).unwrap().unwrap();
        let b = run_events(&mut idle, &mut inbox, BUDGET).unwrap().unwrap();
        assert_eq!(
            (a.payload, b.payload),
            (b"ONE\0".to_vec(), b"TWO\0".to_vec())
        );
        assert_eq!(a.family, FAMILY);
        assert!(run_events(&mut idle, &mut inbox, BUDGET).unwrap().is_none());
    }

    #[test]
    fn run_events_waits_for_a_new_event_and_drops_stray_replies() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![
            Step::Kernel(ack(9)),
            Step::Kernel(data(3, b"LATEREPL")),
            Step::Kernel(event(b"FRESH\0\0\0")),
        ]);
        let ev = run_events(&mut src, &mut inbox, BUDGET).unwrap().unwrap();
        assert_eq!(ev.payload, b"FRESH\0\0\0");
        assert_eq!(inbox.stats().stale_replies, 2);
    }

    #[test]
    fn run_events_ignores_foreign_datagrams_and_wakes_on_enobufs() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![
            Step::Foreign(event(b"FORGED\0\0")),
            Step::Intr,
            Step::Overrun,
        ]);
        assert!(run_events(&mut src, &mut inbox, BUDGET).unwrap().is_none());
        assert_eq!(inbox.stats().foreign_datagrams, 1);
        assert!(inbox.take_events_lost());
        assert_eq!(inbox.pending_events(), 0);
    }

    #[test]
    fn run_events_times_out_quietly() {
        let mut inbox = Inbox::new(PORT);
        let mut src = source(vec![Step::Timeout]);
        assert!(run_events(&mut src, &mut inbox, BUDGET).unwrap().is_none());
        assert!(!inbox.take_events_lost());
    }

    #[test]
    fn events_arriving_while_idle_are_not_lost_by_a_later_request() {
        let mut inbox = Inbox::new(PORT);
        // Idle read queues an event but returns only the first.
        let mut idle = source(vec![Step::Kernel(concat(&[
            event(b"A\0\0\0"),
            event(b"B\0\0\0"),
        ]))]);
        let first = run_events(&mut idle, &mut inbox, BUDGET).unwrap().unwrap();
        assert_eq!(first.payload, b"A\0\0\0");
        let mut src = source(vec![Step::Kernel(ack(2))]);
        run_request(&mut src, &mut inbox, 2, BUDGET).unwrap();
        assert_eq!(inbox.take_event().unwrap().payload, b"B\0\0\0");
    }
}
