/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_GATEWAY_H
#define VPT1_GATEWAY_H

#include <linux/atomic.h>
#include <linux/types.h>

#include "vpt1_acl.h"

/*
 * AY2 gateway runtime: ownership lease, configuration publication, peer table,
 * final admission and teardown, as fixed by docs/AY2_CONTRACT.md. Nothing here
 * parses packets, owns a UDP socket or forwards traffic; those are AY3-AY5.
 *
 * Time is always passed in as boot-time nanoseconds so tests can inject a clock.
 * Locks, outermost first: RTNL, cfg_lock, lease_lock, peer lock (ascending slot),
 * generation sequence locks. Packet paths take only RCU and one peer lock.
 */

#define VPT1_MAX_PEERS 4U
#define VPT1_ID_LEN 16U
#define VPT1_ATTEMPT_ID_LEN 16U
#define VPT1_ATTEMPT_NS (10ULL * NSEC_PER_SEC) /* Whole establishment attempt. */
#define VPT1_ATTEMPT_DONE_NS (20ULL * NSEC_PER_SEC) /* Completed attempt, for answering duplicates. */
/* Liveness (TECHNICAL_DESIGN section 8). */
#define VPT1_PING_NS (5ULL * NSEC_PER_SEC) /* One PING per relationship. */
#define VPT1_PING_FIRST_NS (1ULL * NSEC_PER_SEC) /* The first one, after a new session. */
#define VPT1_CHALLENGE_NS (15ULL * NSEC_PER_SEC) /* A challenge stays answerable this long. */
#define VPT1_LIVE_NS (15ULL * NSEC_PER_SEC) /* No authenticated round trip for this long: closed. */
#define VPT1_CHALLENGES 3U /* Live challenges per peer. */
#define VPT1_CHALLENGE_LEN 16U
#define VPT1_KEY_BYTES 32U
#define VPT1_LEASE_NS (5ULL * NSEC_PER_SEC)
#define VPT1_MAX_RETIRED 2
/*
 * Rekey (AY4): a new generation receives at once but takes over sending after the first
 * record authenticated under it, or this long; the old one still receives for the grace.
 */
#define VPT1_PREV_GRACE_NS (10ULL * NSEC_PER_SEC)
/*
 * Activation (TECHNICAL_DESIGN section 5.2). A staged generation is a candidate: it receives control
 * records but carries no application data until a local commit. The initiator sends CONFIRM, the
 * responder answers READY, the initiator commits and sends COMMIT, the responder commits and sends
 * COMMITTED. Control retries step 0.5, 1, 2 then every 4 s inside the VPT1_ATTEMPT_NS deadline.
 */
#define VPT1_HASH_LEN 32U
#define VPT1_REKEY_PAYLOAD_LEN 48U /* Attempt ID, then the parent handshake hash. */
#define VPT1_REKEY_NEEDED_NS (5ULL * NSEC_PER_SEC) /* Least spacing of REKEY_NEEDED records and events. */
#define VPT1_CFG_BUDGET (1024UL * 1024UL) /* Charged bytes per gateway. */
#define VPT1_PEER_INFLIGHT_MAX 4096
#define VPT1_FRAG_BUDGET (2UL * 1024UL * 1024UL) /* Fragment bytes, all peers of one gateway. */
#define VPT1_JOB_QUEUE_MAX 64 /* Queued jobs per gateway. */
/*
 * Whole-runtime memory budgets (HR4/HR5 handoff, AY5). Every bounded runtime allocation a gateway
 * makes on the data path -- the committed+retired configuration charge, the shared fragment-cache
 * bytes, and the per-peer recent-error rings -- is also counted against one per-gateway ceiling and
 * one host-wide ceiling, so no single gateway and no set of gateways can exhaust kernel memory. The
 * specific per-gateway caps above (CFG, FRAG) still apply first; these are the aggregate backstop.
 * Reserve takes the host ceiling, then the gateway ceiling, and rolls the host charge back if the
 * gateway ceiling is exceeded.
 */
#define VPT1_GW_MEM_BUDGET (16UL * 1024UL * 1024UL) /* All counted bytes of one gateway. */
#define VPT1_HOST_MEM_BUDGET (128UL * 1024UL * 1024UL) /* All counted bytes of every gateway. */
#define VPT1_NEW_FLOW_RATE 100ULL /* Per peer per second; burst twice this. */
#define VPT1_ERROR_RATE 10ULL

struct vpt1_incarnation {
	u8 id[VPT1_ID_LEN];
};

/* One counter space: first the admission reasons, then lifecycle events. */
enum vpt1_reason {
	VPT1_R_OK,
	VPT1_R_STOPPING,
	VPT1_R_NO_OWNER,
	VPT1_R_LEASE_EXPIRED,
	VPT1_R_NO_CONFIG,
	VPT1_R_PEER_INELIGIBLE,
	VPT1_R_STALE_INCARNATION,
	VPT1_R_NO_GENERATION,
	VPT1_R_GENERATION_LIMIT,
	VPT1_R_ACL_DENY,
	VPT1_R_NO_WITNESS,
	VPT1_R_BAD_CONTEXT,
	VPT1_R_RATE_LIMIT,
	VPT1_R_INFLIGHT_LIMIT,
	VPT1_R_COUNT, /* Legacy reasons; events retain their original numeric IDs. */
	VPT1_R_SIZE = 49, /* Appended COUNTERS member 50, after the activation counters. */
};

enum vpt1_event {
	VPT1_E_LEASE_EXPIRED = VPT1_R_COUNT,
	VPT1_E_OWNER_LOST,
	VPT1_E_CLAIMS,
	VPT1_E_RENEWALS,
	VPT1_E_COMMITS,
	VPT1_E_DISRUPTIVE_COMMITS,
	VPT1_E_REJECTED,
	/* Data path. */
	VPT1_E_TX_PACKETS,
	VPT1_E_RX_PACKETS,
	VPT1_E_TX_NO_ROUTE,
	VPT1_E_TX_DROPPED,
	VPT1_E_RX_MALFORMED,
	VPT1_E_RX_UNKNOWN_LOCATOR,
	VPT1_E_RX_BAD_SOURCE,
	VPT1_E_RX_AUTH_FAIL,
	VPT1_E_RX_REPLAY,
	VPT1_E_RX_POLICY,
	VPT1_E_RX_DROPPED,
	VPT1_E_TX_EXCHANGE,
	VPT1_E_RX_EXCHANGE,
	VPT1_E_RX_EXCHANGE_DROPPED,
	VPT1_E_RX_ECN_ANOMALY, /* Appended COUNTERS nested attribute 36; old IDs unchanged. */
	/* Liveness records (appended; old IDs unchanged). */
	VPT1_E_TX_PING,
	VPT1_E_RX_PING,
	VPT1_E_TX_PONG,
	VPT1_E_RX_PONG,
	VPT1_E_RX_CONTROL_DROPPED, /* Authenticated control record that is not acted on. */
	VPT1_E_LIVENESS_LOST, /* A peer closed for want of an authenticated round trip. */
	/* Activation and granted rekey (appended; old IDs unchanged). */
	VPT1_E_ACTIVATED, /* A candidate committed locally. */
	VPT1_E_ACTIVATION_FAILED, /* A candidate expired, or a commit never got COMMITTED. */
	VPT1_E_RX_CANDIDATE_DROPPED, /* Application data under a not yet committed generation. */
	VPT1_E_TX_ACTIVATION, /* CONFIRM, READY, COMMIT and COMMITTED records sent. */
	VPT1_E_RX_ACTIVATION, /* Activation records accepted for the expected state. */
	VPT1_E_REKEY_GRANTS, /* REKEY_START accepted (responder) or REKEY_OK matched (initiator). */
	VPT1_E_REKEY_NEEDED, /* REKEY_NEEDED sent or raised to the controller. */
	/* Index 49 is the SIZE reason (VPT1_R_SIZE, COUNTERS member 50), appended after main's events. */
	/* The live adapter's counters follow the SIZE reason (index 49), COUNTERS members 51... */
	VPT1_E_FRAG_QUEUED = VPT1_R_SIZE + 1, /* A fragment was queued for normalization. */
	VPT1_E_FRAG_COMPLETE, /* A normalized datagram was forwarded or delivered. */
	VPT1_E_FRAG_DROPPED, /* A fragment or a normalized datagram was refused after intake. */
	VPT1_E_FRAG_BUSY, /* A fragment was refused: job queue or shared fragment budget. */
	VPT1_E_ERROR_ADMITTED, /* An associated ICMP error was forwarded, either direction. */
	VPT1_E_ERROR_DENIED, /* An ICMP error with no matching recent context or permission. */
	VPT1_E_TX_GSO_LIMIT, /* A packet beyond 64 in one transmit batch (one GSO aggregate). */
	VPT1_E_COUNT,
};

enum vpt1_dir {
	VPT1_DIR_TX,
	VPT1_DIR_RX,
};

enum vpt1_peer_state {
	VPT1_PEER_DOWN, /* Configured, no generation installed. */
	VPT1_PEER_READY, /* A generation is installed and the peer is eligible. */
	VPT1_PEER_DISABLED, /* Operator disabled; STAGE_GENERATION may re-enable. */
	VPT1_PEER_INVALID, /* Closed for good: superseded, revoked or torn down. */
};

/* Value copies of the controller's decoded configuration; never wire layouts. */
struct vpt1_cfg_peer {
	bool present;
	u8 slot; /* 1..4, the shared slot map. */
	u8 role; /* 0 responder, 1 initiator. */
	u16 pmtu;
	__be32 endpoint;
	__be16 port;
	u8 key[VPT1_KEY_BYTES];
	struct vpt1_acl_owner owned;
};

struct vpt1_snapshot {
	struct vpt1_acl_domain domain;
	u32 lan_ifindex;
	u32 underlay_ifindex;
	__be32 underlay_address; /* This gateway's outer source address. */
	u8 local_key[VPT1_KEY_BYTES];
	struct vpt1_cfg_peer peers[VPT1_MAX_PEERS + 1];
	unsigned int rule_count;
	struct vpt1_acl_rule rules[VPT1_ACL_MAX_RULES];
};

struct vpt1_gw;
struct vpt1_peer;
struct vpt1_cfg;
struct vpt1_generation;

/* Held by the policy layer after it validated a witness; borrowed for one call. */
struct vpt1_witness {
	u32 rule_id;
	bool valid;
	struct vpt1_acl_flow flow; /* ORIGINAL initiating tuple. */
};

struct vpt1_new_initiation {
	bool protocol_validated; /* Data-free original SYN or original datagram. */
	struct vpt1_acl_flow flow;
};

struct vpt1_error_ctx {
	u64 created_ns;
	u32 rule_id;
	struct vpt1_acl_flow flow; /* Original permitted packet, even before a witness. */
	bool held; /* The policy layer holds the associated flow context. */
	bool validated; /* The quoted headers matched it. */
	u8 peer_slot;
};

struct vpt1_admit_ctx {
	struct vpt1_peer *peer;
	struct vpt1_generation *gen;
	const struct vpt1_cfg *cfg;
	u64 revision;
	u64 now_ns;
	u32 rule_id;
	enum vpt1_dir dir;
	bool locked;
	bool authorized;
	bool reply; /* Matched as the reply to an initiation travelling the other way. */
};

struct vpt1_status_peer {
	u8 slot;
	u8 state;
	u8 has_generation;
	u16 pmtu;
	__be32 endpoint;
	__be16 port;
	struct vpt1_incarnation incarnation;
};

struct vpt1_status {
	bool owned;
	bool stopping;
	u64 owner_epoch;
	u64 lease_remaining_ms;
	u64 revision;
	bool has_staged;
	u64 staged_revision;
	struct vpt1_status_peer peers[VPT1_MAX_PEERS];
	unsigned int peer_count;
};

/* Object counts for leak checks; all return to zero after full teardown. */
long vpt1_gw_live(void);
long vpt1_peer_live(void);
long vpt1_cfg_live(void);

int vpt1_gw_init(void);
void vpt1_gw_exit(void); /* Waits for every queued free and RCU callback. */

struct vpt1_gw *vpt1_gw_create(u32 ifindex, u64 now_ns);
void vpt1_gw_lock(struct vpt1_gw *gw); /* cfg_lock */
void vpt1_gw_unlock(struct vpt1_gw *gw);
bool vpt1_gw_get(struct vpt1_gw *gw); /* Handler reference; false once stopping. */
void vpt1_gw_put(struct vpt1_gw *gw);
u32 vpt1_gw_ifindex(const struct vpt1_gw *gw);

/*
 * The calls below need cfg_lock held by the caller. Owner-gated operations take the
 * exact owner triple; ownership is the controller ID, the netlink portid and the
 * namespace the gateway belongs to.
 */
int vpt1_gw_claim(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN], u32 portid,
		  u64 now_ns, u64 *epoch);
int vpt1_gw_renew(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN], u32 portid,
		  u64 revision, u64 now_ns);
int vpt1_gw_check_owner(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN], u32 portid,
			u64 now_ns);
int vpt1_gw_validate(struct vpt1_gw *gw, u64 revision,
		     const struct vpt1_snapshot *snapshot);
int vpt1_gw_commit(struct vpt1_gw *gw, u64 revision, u64 now_ns, bool *disruptive);
int vpt1_gw_status(struct vpt1_gw *gw, u64 now_ns, struct vpt1_status *out);
void vpt1_gw_counters(const struct vpt1_gw *gw, u64 out[VPT1_E_COUNT]);

/*
 * Liveness. A relationship with a current generation sends a PING (a fresh random challenge) every
 * VPT1_PING_NS from the gateway worker and answers every authenticated PING under its current
 * generation with a PONG. Only a PONG that echoes one of at most VPT1_CHALLENGES live challenges
 * counts, once, as a round trip (inbound data or PINGs alone do not). With none for VPT1_LIVE_NS the
 * peer is closed: application admission fails, its continuity dies, and only a new session (a fresh
 * generation install, which replaces even a still-valid current one) opens it again. A lapsed lease
 * silences both sending and answering, so a gateway without a controller goes quiet.
 *
 * challenge: a challenge if one is due and a slot is free, else false. pong: true when it matched.
 * closed: whether the peer has had no round trip for VPT1_LIVE_NS. is_current: whether @gen is the
 * peer's current generation (liveness is not accepted under a next or retiring one).
 */
bool vpt1_peer_live_challenge(struct vpt1_peer *peer, u64 now_ns, u8 out[VPT1_CHALLENGE_LEN]);
bool vpt1_peer_live_pong(struct vpt1_peer *peer, const u8 c[VPT1_CHALLENGE_LEN], u64 now_ns);
bool vpt1_peer_live_closed(struct vpt1_peer *peer, u64 now_ns);
bool vpt1_peer_gen_is_current(struct vpt1_peer *peer, const struct vpt1_generation *gen);
bool vpt1_gw_lease_live(struct vpt1_gw *gw, u64 now_ns);

/*
 * Kernel-issued establishment attempts (TECHNICAL_DESIGN section 5). A peer has at most one pending
 * attempt: an attempt ID and a receive locator the kernel chose (random, nonzero, collision-checked
 * against every retained and pending locator of the gateway), valid VPT1_ATTEMPT_NS from the
 * moment it was issued. A later begin replaces it. All three take cfg_lock.
 *
 * begin: with @given NULL (initiator) the kernel also picks the attempt ID; with @given (responder)
 * the ID must be that of the last INIT the kernel delivered for this peer, within VPT1_ATTEMPT_NS,
 * so the controller cannot make up an attempt for a peer that never sent one (-ENOENT).
 * check: -ENOENT for no such attempt, -ETIMEDOUT past its deadline, -ESTALE for another
 * incarnation (skipped when @inc is NULL), -EINVAL when @local_rx is nonzero and not the issued one.
 * With @staging the attempt must not have installed its keys yet, so keys install once per attempt.
 * done: marks the keys installed. The attempt then stays valid for sending its INIT/RESPONSE (a
 * responder answers duplicate INITs) for VPT1_ATTEMPT_DONE_NS, but can stage nothing more.
 */
int vpt1_gw_attempt_begin(struct vpt1_gw *gw, unsigned int slot, const struct vpt1_incarnation *inc,
			  const u8 *given, u64 now_ns, u8 id[VPT1_ATTEMPT_ID_LEN], u64 *local_rx);
int vpt1_gw_attempt_check(struct vpt1_gw *gw, unsigned int slot, const struct vpt1_incarnation *inc,
			  const u8 id[VPT1_ATTEMPT_ID_LEN], u64 local_rx, u64 now_ns, bool staging);
void vpt1_gw_attempt_done(struct vpt1_gw *gw, unsigned int slot, const u8 id[VPT1_ATTEMPT_ID_LEN],
			  u64 now_ns);
/* An INIT from this peer's endpoint was delivered to the owner (any context). */
void vpt1_peer_note_init(struct vpt1_peer *peer, const u8 id[VPT1_ATTEMPT_ID_LEN], u64 now_ns);

/*
 * Install, disable or revoke by slot; the incarnation must equal the peer's.
 *
 * stage: the generation becomes the peer's one candidate (it replaces an earlier one). It receives
 * control records at once but carries no application data until committed (below). @hash is the
 * full handshake hash both sides bind CONFIRM, READY, COMMIT and COMMITTED to. @mode 0 is a fresh
 * establishment: the commit replaces the whole session (new continuity, old keys dropped). @mode 1
 * keeps the session's continuity and needs a live grant for @attempt whose parent is the current
 * generation's hash, @parent (-EPERM otherwise, and the attempt stays unconsumed).
 * duplicate: >0 when the attempt was already staged with the same hash and locators (acknowledge,
 * change nothing), 0 when it was not staged, -EEXIST when staged differently.
 */
int vpt1_gw_stage_generation(struct vpt1_gw *gw, unsigned int slot,
			     const struct vpt1_incarnation *inc,
			     struct vpt1_generation *gen, u64 local_rx, u64 remote_rx,
			     const u8 hash[VPT1_HASH_LEN], u8 mode,
			     const u8 attempt[VPT1_ATTEMPT_ID_LEN], const u8 *parent);
int vpt1_gw_stage_duplicate(struct vpt1_gw *gw, unsigned int slot,
			    const struct vpt1_incarnation *inc,
			    const u8 attempt[VPT1_ATTEMPT_ID_LEN], u64 local_rx, u64 remote_rx,
			    const u8 hash[VPT1_HASH_LEN]);
/*
 * Initiator only: ask the responder for a continuity grant (REKEY_START under the current
 * generation, retried inside VPT1_ATTEMPT_NS). The ack means the request is under way; the grant
 * arrives as the REKEY_GRANTED event, after which STAGE_GENERATION mode 1 may follow.
 */
int vpt1_gw_rekey_request(struct vpt1_gw *gw, unsigned int slot,
			  const struct vpt1_incarnation *inc,
			  const u8 attempt[VPT1_ATTEMPT_ID_LEN], u64 now_ns);
int vpt1_gw_disable_peer(struct vpt1_gw *gw, unsigned int slot,
			 const struct vpt1_incarnation *inc);
int vpt1_gw_revoke_peer(struct vpt1_gw *gw, unsigned int slot,
			const struct vpt1_incarnation *inc);

/* Event counters and the owner's portid (for unicast events); any context. */
void vpt1_gw_count_rejected(struct vpt1_gw *gw);
int vpt1_gw_owner_portid(struct vpt1_gw *gw, u32 *portid);
/* Phase A for every gateway at once, ahead of module-exit waits. */
void vpt1_gw_close_all(void);

/* Lease expiry bookkeeping; a clock comparison already denied admission. */
void vpt1_gw_expire_check(struct vpt1_gw *gw, u64 now_ns);

/*
 * Netlink socket release for @portid. Any process context, no sleeping lock.
 * Returns true if it closed the live lease (loss work queued).
 */
bool vpt1_gw_owner_released(struct vpt1_gw *gw, u32 portid);

/* Phase A of teardown: close admission. Non-sleeping. */
void vpt1_gw_close(struct vpt1_gw *gw);
/*
 * Phases A, C and D: close, drain, invalidate, unpublish. Process context, no
 * cfg_lock held. @held is the number of handler references the caller itself
 * still holds, which are not waited for. Consumes the creator's reference.
 */
void vpt1_gw_destroy(struct vpt1_gw *gw, unsigned int held);

/* Peers: counted references; a peer keeps its gateway's memory alive only. */
struct vpt1_peer *vpt1_gw_peer_get(struct vpt1_gw *gw, unsigned int slot);
void vpt1_peer_put(struct vpt1_peer *peer);
void vpt1_peer_incarnation(struct vpt1_peer *peer, struct vpt1_incarnation *out);
/*
 * A peer holds up to three generations: current (sends and receives), the candidate (receives
 * control records; it becomes current only by a local commit) and prev (receives only, for
 * VPT1_PREV_GRACE_NS after it was replaced). Application data is sent under current only, so a
 * replaced or uncommitted key never carries traffic. Each getter returns a referenced generation
 * or NULL.
 */
struct vpt1_generation *vpt1_peer_tx_generation_get(struct vpt1_peer *peer, u64 now_ns,
						    u64 *remote_rx);
/* The candidate and the locator to address it by, for CONFIRM and READY. */
struct vpt1_generation *vpt1_peer_candidate_tx_get(struct vpt1_peer *peer, u64 *remote_rx);
struct vpt1_generation *vpt1_peer_rx_generation_get(struct vpt1_peer *peer, u64 locator,
						    u64 now_ns);
bool vpt1_peer_gen_is_candidate(struct vpt1_peer *peer, const struct vpt1_generation *gen);
/* Commit the candidate locally (promote it). False when there is none. Used by the protocol and tests. */
bool vpt1_peer_commit(struct vpt1_peer *peer, u64 now_ns);

/* What an authenticated control record asks of the caller, once the peer lock is dropped. */
enum vpt1_send_gen {
	VPT1_SEND_CURRENT,
	VPT1_SEND_CANDIDATE,
};
enum vpt1_ctl_event {
	VPT1_EVT_NONE,
	VPT1_EVT_REKEY_GRANTED, /* Initiator: the responder granted the attempt. */
	VPT1_EVT_REKEY_NEEDED, /* Start a re-key: the responder asked, or limits are near. */
};
struct vpt1_ctl_out {
	u8 subtype; /* A record to send, or 0. */
	enum vpt1_send_gen which;
	u8 payload[VPT1_REKEY_PAYLOAD_LEN];
	unsigned int len;
	enum vpt1_ctl_event event;
	u8 attempt[VPT1_ATTEMPT_ID_LEN];
};
/*
 * An authenticated CONFIRM, READY, COMMIT, COMMITTED, REKEY_START, REKEY_OK or REKEY_NEEDED record
 * under @gen. All state-changing controls are accepted only for their expected generation, role,
 * state and correlation value; handlers are idempotent. 0: acted on (maybe with a reply in @out),
 * -EPROTO: not for this state (counted by the caller as dropped). Any context.
 */
int vpt1_peer_control_rx(struct vpt1_peer *peer, struct vpt1_generation *gen, u8 subtype,
			 const u8 *payload, unsigned int len, u64 now_ns, struct vpt1_ctl_out *out);

/*
 * Continuity (the fragment cache's canonical context, docs/HR3.md). One object per established peer
 * session. An ordinary rekey keeps it; a fresh establishment (first install, or an install after the
 * keys hit their hard limit), disable, revoke and teardown kill it and, for a fresh establishment,
 * replace it. A dead one never hands out another reference, so a fragment set that outlives it can
 * only be flushed, never completed. try_get and put are the vpt1_frag_ops get and put.
 */
struct vpt1_continuity;
struct vpt1_continuity *vpt1_peer_continuity_get(struct vpt1_peer *peer);
bool vpt1_continuity_try_get(void *continuity);
void vpt1_continuity_put(void *continuity);
u64 vpt1_continuity_id(const struct vpt1_continuity *continuity);
bool vpt1_continuity_dead(const struct vpt1_continuity *continuity);
long vpt1_continuity_live(void);

/*
 * Shared fragment budget, the vpt1_frag_ops reserve and release with the gateway as budget owner.
 * Nonblocking, never takes a lock, safe under the cache mutex. The owner must outlive its caches.
 */
bool vpt1_gw_frag_reserve(void *gateway, size_t charge);
void vpt1_gw_frag_release(void *gateway, size_t charge);
size_t vpt1_gw_frag_used(struct vpt1_gw *gw);

/*
 * Aggregate runtime-memory reservation (16 MiB/gateway, 128 MiB/host). Nonblocking and lock-free,
 * safe in any context. Returns true and holds @bytes against both ceilings, or false and holds
 * nothing. The fragment budget and the per-peer error rings both reserve through this; release
 * returns exactly what reserve took. _used readers are for STATUS and tests.
 */
bool vpt1_gw_mem_reserve(struct vpt1_gw *gw, size_t bytes);
void vpt1_gw_mem_release(struct vpt1_gw *gw, size_t bytes);
size_t vpt1_gw_mem_used(struct vpt1_gw *gw);
size_t vpt1_host_mem_used(void);

/*
 * Bounded worker queue: the process-context step the live adapter hands fragments to
 * (docs/HR3_LIVE.md). run() is called exactly once, either on the gateway worker with cancelled
 * false, or with cancelled true when the peer is drained (disable-free revoke, config replace,
 * teardown) before it ran; the cancelled call is made with the config lock held and must only
 * free what the job owns. run() never takes the config lock and never queues a job of its own
 * into a stopping gateway (that is refused). It may enter peer admission (the peer lock is a
 * leaf spinlock) once it has released every cache mutex.
 * The job stays valid until run() returns; run() frees it. vpt1_job_queue() takes a peer reference
 * for the queued job and returns -EBUSY when the queue is full, -ESTALE for a closed peer and
 * -ESHUTDOWN for a stopping gateway; on an error the caller still owns the job.
 */
struct vpt1_job {
	struct list_head node;
	struct vpt1_peer *peer; /* Set by vpt1_job_queue(). */
	void (*run)(struct vpt1_job *job, bool cancelled);
};
int vpt1_job_queue(struct vpt1_peer *peer, struct vpt1_job *job);
unsigned int vpt1_job_pending(struct vpt1_gw *gw);

/*
 * Admission (contract section 6). The caller holds rcu_read_lock() and a peer
 * reference. begin takes the peer lock on success and keeps it until handoff or
 * abort. No crypto, sleeping allocation or drain may run in between. Handoff
 * reserves bounded in-flight work once; it is not permission to skip a later
 * final output cut. TX rechecks after crypto/routing, RX hands directly to the
 * stack. Output or drop then owes exactly one vpt1_admit_done().
 */
int vpt1_admit_begin(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
		     enum vpt1_dir dir, struct vpt1_generation *gen, u64 now_ns,
		     struct vpt1_admit_ctx *ctx, enum vpt1_reason *reason);
int vpt1_admit_existing(struct vpt1_admit_ctx *ctx, const struct vpt1_witness *w,
			enum vpt1_reason *reason);
int vpt1_admit_new(struct vpt1_admit_ctx *ctx, const struct vpt1_new_initiation *init,
		   enum vpt1_reason *reason);
int vpt1_admit_error(struct vpt1_admit_ctx *ctx, const struct vpt1_error_ctx *err,
		     enum vpt1_reason *reason);
void vpt1_admit_handoff(struct vpt1_admit_ctx *ctx);
void vpt1_admit_abort(struct vpt1_admit_ctx *ctx);
void vpt1_admit_done(struct vpt1_peer *peer);

/*
 * Data-path hooks. The runtime binds one UDP socket per gateway: bind runs in process
 * context under cfg_lock when a configuration whose outer address is new is about to
 * be published (an error aborts the commit), unbind during teardown. Unset in the
 * core test module.
 */
struct vpt1_gw_ops {
	int (*bind)(struct vpt1_gw *gw, __be32 local, u32 ifindex);
	void (*unbind)(struct vpt1_gw *gw);
	/* Process context, RTNL before cfg_lock. Failure preserves the old MTU. */
	int (*set_mtu)(struct vpt1_gw *gw, u16 mtu);
	/* Seal and send one encrypted control record under the peer's current or candidate generation. Any context. */
	int (*send_control)(struct vpt1_gw *gw, struct vpt1_peer *peer, enum vpt1_send_gen which,
			    u8 subtype, const u8 *payload, unsigned int len);
	/* Tell the owning controller to start a re-key, or that a grant arrived. Any context. */
	void (*control_event)(struct vpt1_gw *gw, struct vpt1_peer *peer, enum vpt1_ctl_event event,
			      const u8 *attempt);
};
void vpt1_gw_set_ops(const struct vpt1_gw_ops *ops);
struct net;
void vpt1_gw_set_net(struct vpt1_gw *gw, struct net *net);
struct net *vpt1_gw_net(const struct vpt1_gw *gw);
void vpt1_gw_set_priv(struct vpt1_gw *gw, void *priv);
void *vpt1_gw_priv(const struct vpt1_gw *gw);
void vpt1_gw_stat_inc(struct vpt1_gw *gw, unsigned int which);

/* Fields of one inner IPv4 packet the policy needs; ports are network order. */
struct vpt1_pkt_info {
	__be32 src;
	__be32 dst;
	__be16 sport;
	__be16 dport;
	u8 proto;
	u8 icmp_type;
	u8 icmp_code;
	u8 tcp_flags; /* TCP only: the flags byte of the validated header. */
};

struct vpt1_route {
	__be32 endpoint; /* The peer's outer address. */
	__be32 local; /* This gateway's outer address. */
	u32 ifindex; /* The underlay interface. */
	__be16 port;
	u8 slot;
};

/* RCU read side. The peer owning pi->dst, or -ENOENT; no admission is implied. */
int vpt1_gw_tx_route(struct vpt1_gw *gw, const struct vpt1_pkt_info *pi,
		     struct vpt1_route *route);
/* Outer address, port and interface of one configured peer slot. RCU read side. */
int vpt1_gw_peer_route(struct vpt1_gw *gw, unsigned int slot, struct vpt1_route *route);
/* The configured peer with this outer endpoint, referenced, or NULL. RCU read side. */
struct vpt1_peer *vpt1_gw_peer_by_endpoint(struct vpt1_gw *gw, __be32 addr, __be16 port);
/* Rate limit for unauthenticated INIT/RESPONSE (1/s, burst 2 per peer). */
bool vpt1_peer_exchange_admit(struct vpt1_peer *peer, u64 now_ns);
/* The peer holding this receive locator, referenced, or NULL. */
struct vpt1_peer *vpt1_gw_peer_by_locator(struct vpt1_gw *gw, u64 locator);
/* Is this outer source the peer's configured endpoint? RCU read side. */
bool vpt1_peer_source_ok(struct vpt1_peer *peer, __be32 saddr, __be16 sport);
u8 vpt1_peer_slot(const struct vpt1_peer *peer);
struct vpt1_gw *vpt1_peer_gw(const struct vpt1_peer *peer);
/*
 * Step 2 of admission for one inner packet: the stateless form of the ACL (the packet
 * is an initiation the rules allow, or the reply to one in the opposite direction).
 * Witness tables replace the reply half when HR2 is integrated.
 */
int vpt1_admit_packet(struct vpt1_admit_ctx *ctx, const struct vpt1_pkt_info *pi,
		      enum vpt1_reason *reason);
/* TX final cut after crypto/route work, immediately before UDP output. Caller
 * holds RCU, peer/generation references and exactly one outstanding in-flight
 * handoff plus a successful counter reservation. Rechecks current
 * policy/lease/incarnation/TX generation and size,
 * without reserving a second in-flight slot. Caller still owes exactly one
 * admit_done on success AND failure. No crypto/allocation follows this check.
 * This is the current thin/stateless packet policy, not a live witness adapter;
 * a future witness path must also recheck its exact saved rule and identity.
 */
int vpt1_admit_tx_recheck(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
			  struct vpt1_generation *gen, const struct vpt1_pkt_info *pi,
			  unsigned int length, u64 now_ns, enum vpt1_reason *reason);
/* PING/PONG processing reservation and final cut; no application policy grant.
 * begin returns an authorized locked ctx for handoff; recheck requires that
 * reservation and a successful counter reservation. One done on output/drop.
 */
int vpt1_admit_control_begin(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
			     struct vpt1_generation *gen, u64 now_ns,
			     struct vpt1_admit_ctx *ctx, enum vpt1_reason *reason);
int vpt1_admit_control_recheck(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
			       struct vpt1_generation *gen, u64 now_ns,
			       enum vpt1_reason *reason);
/* Current peer cap under the same held admission lock; rejects/unlocks on oversize. */
int vpt1_admit_size(struct vpt1_admit_ctx *ctx, unsigned int length,
		    enum vpt1_reason *reason);
/* RCU-borrowed lookup only; no authentication or eligibility is granted. */
bool vpt1_gw_underlay_local(struct vpt1_gw *gw, u32 ifindex, __be32 destination);

/*
 * Live IP adapter (HR3/HR4): fragment normalization and associated ICMP errors.
 * Intake checks (RCU read side). A fragment has no ports, so ownership is by address only: LAN to
 * tunnel the source is on this site's LAN (not the gateway's own address, unless
 * @allow_gateway_address: the source of its own errors) and the destination belongs to the peer;
 * tunnel to LAN the source belongs to the authenticated peer and the destination is on the LAN.
 */
bool vpt1_gw_lan_has(struct vpt1_gw *gw, __be32 addr, bool allow_gateway_address);
bool vpt1_peer_owns(struct vpt1_peer *peer, __be32 addr);
/* The pair's inner cap (pair PMTU minus the 72-byte expansion), 0 when unconfigured. */
unsigned int vpt1_peer_cap(struct vpt1_peer *peer);
/* Cheap pre-queue eligibility: owner lease live, peer READY and not closed for liveness. */
bool vpt1_peer_eligible(struct vpt1_peer *peer, u64 now_ns);

struct vpt1_frag_cache;
/*
 * The peer's fragment cache, created on first use. Worker context only (it sleeps), and only for a
 * peer that is not INVALID: the drain that precedes every peer free waits for the running job and
 * destroys it. Returns ERR_PTR on failure. vpt1_gw_frag_work_arm() schedules the periodic expiry
 * (once a second while any set may be held); the work runs on the ordered gateway worker.
 */
struct vpt1_frag_cache *vpt1_peer_frag_cache(struct vpt1_peer *peer);
void vpt1_gw_frag_work_arm(struct vpt1_gw *gw);

/* Recent-packet contexts for associated ICMP errors (TECHNICAL_DESIGN 10.3), 256 per direction. */
struct vpt1_error_ctx;
/* Remember an admitted packet. @saved_in is its direction (TX: sent to the peer; RX: received). */
void vpt1_peer_err_save(struct vpt1_peer *peer, enum vpt1_dir saved_in, u32 rule_id, bool reply,
			const u8 *packet, size_t length, u64 now_ns);
/*
 * The unique recent packet an ICMP error answers. @error travels opposite to @saved_in (an error
 * to the peer answers a packet we received). 0 and *out filled; -ENOENT for no match; -ENOTUNIQ
 * when the matches disagree on their rule; -EINVAL for a malformed request.
 */
int vpt1_peer_err_find(struct vpt1_peer *peer, enum vpt1_dir saved_in, const u8 *error,
		       size_t length, u64 now_ns, struct vpt1_error_ctx *out);
/* TX final cut for an error, as vpt1_admit_tx_recheck() is for a packet: no second reservation. */
int vpt1_admit_tx_recheck_error(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
				struct vpt1_generation *gen, const struct vpt1_error_ctx *err,
				unsigned int length, u64 now_ns, enum vpt1_reason *reason);

/* Wait for queued expiry/loss work (tests and module exit). */
void vpt1_gw_flush(void);

#ifdef VPT1_DEBUG
/* Read-only test observation, cfg_lock held; no raw cfg pointer escapes. */
struct vpt1_cfg_accounting {
	size_t charged;
	size_t current_charge;
	size_t staged_charge;
	size_t cfg_bucket;
	unsigned int retired;
	size_t gw_mem; /* All counted bytes of this gateway (<= VPT1_GW_MEM_BUDGET). */
	size_t host_mem; /* All counted bytes of every gateway (<= VPT1_HOST_MEM_BUDGET). */
};

void vpt1_gw_debug_accounting(struct vpt1_gw *gw, struct vpt1_cfg_accounting *out);

/* Adjust the retired-config count, to exercise the ceiling deterministically. */
void vpt1_gw_dbg_add_retired(struct vpt1_gw *gw, int delta);
/* Stage like the pre-activation API did (see gateway.c). cfg_lock held. */
int vpt1_gw_dbg_stage(struct vpt1_gw *gw, unsigned int slot, const struct vpt1_incarnation *inc,
		      struct vpt1_generation *gen, u64 local_rx, u64 remote_rx);
/* Test knobs, compiled out of release builds. */
extern atomic_t vpt1_dbg_stall_expiry; /* Non-zero: the expiry/loss work waits. */
extern unsigned int vpt1_dbg_post_handoff_us; /* Delay inside vpt1_admit_handoff(). */
extern atomic_t vpt1_dbg_fail_nth; /* Fail the Nth gateway allocation from now (1-based); 0 is off. */
extern atomic_t vpt1_dbg_fail_hit; /* Set when an allocation was failed that way. */
#endif

#endif /* VPT1_GATEWAY_H */
