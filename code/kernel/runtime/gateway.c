// SPDX-License-Identifier: GPL-2.0-only
/*
 * AY2 gateway runtime: lease, configuration publication, peer table, final
 * admission and teardown. See docs/AY2_CONTRACT.md; section numbers below refer
 * to it. No packet, socket or device code lives here.
 */
#include <linux/delay.h>
#include <linux/icmp.h>
#include <linux/bitmap.h>
#include <linux/in.h>
#include <net/ip.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>

#include "vpt1_gateway.h"
#include "vpt1_generation.h"
#include "vpt1_ip.h"
#include "vpt1_wire.h"
#include "vpt1_error.h"
#include "vpt1_fragment.h"

#ifdef VPT1_DEBUG
atomic_t vpt1_dbg_stall_expiry = ATOMIC_INIT(0);
unsigned int vpt1_dbg_post_handoff_us;
atomic_t vpt1_dbg_fail_nth = ATOMIC_INIT(0);
atomic_t vpt1_dbg_fail_hit = ATOMIC_INIT(0);
#endif

/*
 * Every allocation of a gateway, its accounting, a config, a peer or a loss record goes through here, so
 * a debug build can fail the Nth one (vpt1_dbg_fail_nth, counted from 1 and spent when it fires) and the
 * allocation-failure test can walk every failure point. Release builds are plain kzalloc().
 */
static void *gw_zalloc(size_t size, gfp_t gfp)
{
#ifdef VPT1_DEBUG
	if (atomic_read(&vpt1_dbg_fail_nth) && atomic_dec_return(&vpt1_dbg_fail_nth) == 0) {
		atomic_set(&vpt1_dbg_fail_hit, 1);
		return NULL;
	}
#endif
	return kzalloc(size, gfp);
}

/* As gw_zalloc() for the larger per-peer tables (may use vmalloc); freed with kvfree(). */
static void *gw_kvzalloc(size_t size)
{
#ifdef VPT1_DEBUG
	if (atomic_read(&vpt1_dbg_fail_nth) && atomic_dec_return(&vpt1_dbg_fail_nth) == 0) {
		atomic_set(&vpt1_dbg_fail_hit, 1);
		return NULL;
	}
#endif
	return kvzalloc(size, GFP_KERNEL);
}

/* Shared accounting: referenced by the gateway and by every config, so a
 * config's RCU callback never needs the gateway object.
 */
struct vpt1_acct {
	refcount_t refs;
	atomic_t retired;
	atomic_long_t charged;
	atomic_long_t frag; /* Fragment cache bytes, all peers (vpt1_gw_frag_reserve). */
	atomic_long_t mem; /* All counted bytes of this gateway (cfg + frag + error rings). */
};

struct vpt1_continuity {
	refcount_t refs; /* The peer's, plus one per borrower. */
	atomic_t dead;
	u64 id;
};

struct vpt1_cfg {
	struct rcu_head rcu;
	struct vpt1_acct *acct;
	struct vpt1_acl_snapshot *acl; /* Exclusively owned; freed with the cfg. */
	u64 revision;
	u64 owner_epoch;
	size_t charge;
	u32 lan_ifindex;
	u32 underlay_ifindex;
	__be32 underlay_address;
	u8 local_key[VPT1_KEY_BYTES];
	struct vpt1_acl_prefix lan;
	struct vpt1_acl_prefix underlay;
	__be32 lan_address;
	struct vpt1_cfg_peer peers[VPT1_MAX_PEERS + 1];
};

struct vpt1_bucket {
	u64 milli; /* Tokens times 1000. */
	u64 last_ns;
};

/*
 * Recent admitted packets, one ring per direction, for associated ICMP errors (docs/HR4_ERRORS.md).
 * 256 entries of 80 bytes each way, so 40 KiB per peer, allocated once with the peer. The lock is a
 * leaf taken in softirq and process context; nothing is allocated or called while it is held.
 */
struct vpt1_err_dir {
	struct vpt1_recent_packet slot[VPT1_ERROR_CONTEXTS];
	DECLARE_BITMAP(reply, VPT1_ERROR_CONTEXTS); /* The ACL matched the entry as a reply. */
	unsigned int next;
};

struct vpt1_err_table {
	spinlock_t lock; /* Leaf: guards both rings. */
	struct vpt1_err_dir dir[2];
};

enum {
	ACT_NONE,
	ACT_WAIT_READY, /* Initiator: CONFIRM sent under the candidate. */
	ACT_WAIT_COMMITTED, /* Initiator: committed locally, COMMIT sent. */
};

enum {
	RK_NONE,
	RK_REQUESTED, /* Initiator: REKEY_START sent, no REKEY_OK yet. */
	RK_GRANTED,
};

struct vpt1_peer {
	spinlock_t lock;
	refcount_t refs;
	struct rcu_head rcu;
	struct vpt1_gw *gw; /* Holds a life reference. */
	u8 slot;
	u8 state;
	u64 owner_epoch;
	struct vpt1_incarnation inc;
	struct vpt1_generation *gen; /* Current: sends and receives. */
	u64 local_rx; /* Receive locator this peer's generation is addressed by. */
	u64 remote_rx; /* Locator to put in the envelope we send. */
	/* Rekey (AY4), all under the peer lock; the locators are read locklessly for demux. */
	struct vpt1_generation *next_gen; /* The candidate: control only, until a local commit. */
	u64 next_local_rx;
	u64 next_remote_rx;
	u64 next_deadline_ns; /* The candidate is dropped at this time if not committed. */
	/*
	 * Activation (section 5.2), under the peer lock. The candidate is a fresh establishment
	 * (next_fresh, with its own continuity) or the continuity-keeping successor of the current one.
	 */
	u8 role; /* 0 responder, 1 initiator. */
	u8 next_hash[VPT1_HASH_LEN];
	u8 next_att[VPT1_ATTEMPT_ID_LEN];
	bool next_fresh;
	struct vpt1_continuity *next_cont;
	u8 gen_hash[VPT1_HASH_LEN]; /* The current generation's handshake hash and attempt. */
	u8 gen_att[VPT1_ATTEMPT_ID_LEN];
	bool act_closed; /* A commit never got COMMITTED: closed until a new session. */
	u8 act_state; /* ACT_*. */
	u64 act_due_ns; /* Next CONFIRM / COMMIT retry. */
	unsigned int act_step;
	u64 act_deadline_ns; /* Whole activation. */
	/* Continuity grant (responder) and its request (initiator), section 6. */
	bool grant_valid;
	bool grant_used;
	u8 grant_att[VPT1_ATTEMPT_ID_LEN];
	u8 grant_parent[VPT1_HASH_LEN];
	u64 grant_deadline_ns;
	u8 rk_state; /* RK_*. */
	u8 rk_att[VPT1_ATTEMPT_ID_LEN];
	u8 rk_parent[VPT1_HASH_LEN];
	u64 rk_due_ns;
	unsigned int rk_step;
	u64 rk_deadline_ns;
	u64 needed_ns; /* Last REKEY_NEEDED sent or raised. */
	struct vpt1_continuity *cont; /* Fragment cache context; peer lock. */
	/* Live IP adapter. frag is created by the gateway worker and destroyed by peer_drain(). */
	struct vpt1_frag_cache *frag;
	struct vpt1_err_table *errs;
	size_t errs_charge; /* Aggregate-budget bytes held for errs; released in peer_free. */
	struct vpt1_generation *prev_gen; /* Receives only, until prev_deadline_ns. */
	u64 prev_local_rx;
	u64 prev_deadline_ns;
	/* Kernel-issued attempt (vpt1_gw_attempt_begin) and the last INIT delivered; peer lock. */
	bool att_valid;
	bool att_done; /* Keys installed. */
	u8 att_id[VPT1_ATTEMPT_ID_LEN];
	u64 att_rx;
	u64 att_deadline_ns;
	bool init_valid;
	u8 init_id[VPT1_ATTEMPT_ID_LEN];
	u64 init_deadline_ns;
	/* Liveness, peer lock. */
	bool live_armed;
	bool live_lost; /* The closure has been acted on. */
	u64 live_last_ns;
	u64 ping_due_ns;
	struct {
		u8 c[VPT1_CHALLENGE_LEN];
		u64 expiry_ns;
	} chal[VPT1_CHALLENGES];
	atomic_t inflight;
	struct vpt1_bucket new_flows;
	struct vpt1_bucket errors;
	struct vpt1_bucket exchange;
};

struct vpt1_gw {
	refcount_t life; /* Memory: creator, peers. */
	refcount_t users; /* Handlers and workers; destroy waits for these. */
	struct rcu_head rcu;
	u32 ifindex;
	struct mutex cfg_lock;
	spinlock_t lease_lock;
	/* Written under lease_lock; admission reads the two words with READ_ONCE. */
	u64 live_epoch;
	u64 deadline_ns;
	u64 next_epoch;
	u8 controller_id[VPT1_ID_LEN];
	u32 portid;
	bool stopping;
	struct vpt1_cfg __rcu *cfg;
	struct vpt1_cfg *staged;
	u64 staged_epoch;
	struct vpt1_peer __rcu *peers[VPT1_MAX_PEERS + 1];
	struct vpt1_acct *acct;
	struct net *net;
	void *priv;
	bool bound;
	atomic64_t ctr[VPT1_E_COUNT];
	struct delayed_work expiry_work;
	struct delayed_work gens_work; /* Wipes retired generations after their grace. */
	struct delayed_work live_work; /* PINGs, and acts on a closed peer. */
	struct delayed_work frag_work; /* Expires stale fragment sets while a cache may hold one. */
	struct delayed_work act_work; /* Activation and rekey-grant retries, candidate expiry. */
	/* Bounded job queue (vpt1_job_queue); job_lock is a leaf. */
	spinlock_t job_lock;
	struct list_head jobs;
	unsigned int job_count;
	bool jobs_closed;
	struct vpt1_peer *job_running;
	struct work_struct jobs_work;
	struct list_head node;
};

struct vpt1_loss {
	struct work_struct work;
	struct vpt1_gw *gw; /* Holds a users reference. */
	u64 epoch;
};

static struct workqueue_struct *vpt1_gw_wq;
static const struct vpt1_gw_ops *vpt1_gw_ops_ptr;
static LIST_HEAD(vpt1_gw_list);
static DEFINE_SPINLOCK(vpt1_gw_list_lock);
static atomic_long_t vpt1_gw_count = ATOMIC_LONG_INIT(0);
static atomic_long_t vpt1_peer_count = ATOMIC_LONG_INIT(0);
static atomic_long_t vpt1_cfg_count = ATOMIC_LONG_INIT(0);
static atomic_long_t vpt1_cont_count = ATOMIC_LONG_INIT(0);
/* Counted runtime bytes of every gateway on this host (vpt1_gw_mem_reserve). */
static atomic_long_t vpt1_host_mem = ATOMIC_LONG_INIT(0);
static atomic64_t vpt1_cont_id = ATOMIC64_INIT(0);

long vpt1_continuity_live(void)
{
	return atomic_long_read(&vpt1_cont_count);
}

long vpt1_gw_live(void)
{
	return atomic_long_read(&vpt1_gw_count);
}

long vpt1_peer_live(void)
{
	return atomic_long_read(&vpt1_peer_count);
}

long vpt1_cfg_live(void)
{
	return atomic_long_read(&vpt1_cfg_count);
}

int vpt1_gw_init(void)
{
	if (vpt1_gw_wq)
		return -EBUSY;
	vpt1_gw_wq = alloc_ordered_workqueue("vpt1_gw", 0);
	return vpt1_gw_wq ? 0 : -ENOMEM;
}

void vpt1_gw_exit(void)
{
	if (vpt1_gw_wq)
		destroy_workqueue(vpt1_gw_wq);
	vpt1_gw_wq = NULL;
	/*
	 * Every cfg and peer free was queued before this point, and the last peer
	 * callback queues the gateway free: a second barrier waits for that one.
	 */
	rcu_barrier();
	rcu_barrier();
}

void vpt1_gw_set_ops(const struct vpt1_gw_ops *ops)
{
	vpt1_gw_ops_ptr = ops;
}

void vpt1_gw_set_net(struct vpt1_gw *gw, struct net *net)
{
	gw->net = net;
}

struct net *vpt1_gw_net(const struct vpt1_gw *gw)
{
	return gw->net;
}

void vpt1_gw_set_priv(struct vpt1_gw *gw, void *priv)
{
	WRITE_ONCE(gw->priv, priv);
}

void *vpt1_gw_priv(const struct vpt1_gw *gw)
{
	return READ_ONCE(gw->priv);
}

void vpt1_gw_stat_inc(struct vpt1_gw *gw, unsigned int which)
{
	atomic64_inc(&gw->ctr[which]);
}

void vpt1_gw_flush(void)
{
	if (vpt1_gw_wq)
		flush_workqueue(vpt1_gw_wq);
}

#ifdef VPT1_DEBUG
void vpt1_gw_dbg_add_retired(struct vpt1_gw *gw, int delta)
{
	atomic_add(delta, &gw->acct->retired);
}
#endif

static void ctr_inc(struct vpt1_gw *gw, unsigned int which)
{
	atomic64_inc(&gw->ctr[which]);
}

static void reason_inc(struct vpt1_gw *gw, enum vpt1_reason reason)
{
	ctr_inc(gw, reason);
}

/* Accounting --------------------------------------------------------------- */

static struct vpt1_acct *acct_alloc(void)
{
	struct vpt1_acct *acct = gw_zalloc(sizeof(*acct), GFP_KERNEL);

	if (acct) {
		refcount_set(&acct->refs, 1);
		atomic_set(&acct->retired, 0);
		atomic_long_set(&acct->charged, 0);
		atomic_long_set(&acct->frag, 0);
		atomic_long_set(&acct->mem, 0);
	}
	return acct;
}

static void acct_put(struct vpt1_acct *acct)
{
	if (refcount_dec_and_test(&acct->refs))
		kfree(acct);
}

/* Configs ------------------------------------------------------------------ */

static void cfg_destroy(struct vpt1_cfg *cfg, bool retired)
{
	vpt1_acl_destroy(cfg->acl);
	atomic_long_sub(cfg->charge, &cfg->acct->charged);
	/*
	 * Release the aggregate charge too. This runs from the cfg's RCU callback with only the
	 * shared acct in hand (never the gateway), so it subtracts from acct->mem and the host
	 * accumulator directly rather than through vpt1_gw_mem_release(gw, ...).
	 */
	if (WARN_ON_ONCE(atomic_long_sub_return(cfg->charge, &cfg->acct->mem) < 0))
		atomic_long_set(&cfg->acct->mem, 0);
	if (WARN_ON_ONCE(atomic_long_sub_return(cfg->charge, &vpt1_host_mem) < 0))
		atomic_long_set(&vpt1_host_mem, 0);
	if (retired)
		atomic_dec(&cfg->acct->retired);
	acct_put(cfg->acct);
	atomic_long_dec(&vpt1_cfg_count);
	kfree(cfg);
}

static void cfg_free(struct rcu_head *rcu)
{
	struct vpt1_cfg *cfg = container_of(rcu, struct vpt1_cfg, rcu);

	cfg_destroy(cfg, true);
}

/* Never published: cfg_lock excludes all staged readers; no grace period. */
static void cfg_discard(struct vpt1_cfg *cfg)
{
	if (cfg)
		cfg_destroy(cfg, false);
}

static void cfg_retire(struct vpt1_cfg *cfg)
{
	if (!cfg)
		return;
	atomic_inc(&cfg->acct->retired);
	call_rcu(&cfg->rcu, cfg_free);
}

#ifdef VPT1_DEBUG
void vpt1_gw_debug_accounting(struct vpt1_gw *gw, struct vpt1_cfg_accounting *out)
{
	struct vpt1_cfg *cfg;

	lockdep_assert_held(&gw->cfg_lock);
	cfg = rcu_dereference_protected(gw->cfg, lockdep_is_held(&gw->cfg_lock));
	out->charged = atomic_long_read(&gw->acct->charged);
	out->retired = atomic_read(&gw->acct->retired);
	out->current_charge = cfg ? cfg->charge : 0;
	out->staged_charge = gw->staged ? gw->staged->charge : 0;
	out->cfg_bucket = kmalloc_size_roundup(sizeof(*cfg));
	out->gw_mem = atomic_long_read(&gw->acct->mem);
	out->host_mem = atomic_long_read(&vpt1_host_mem);
}
#endif

/* Peers -------------------------------------------------------------------- */

static void gw_free(struct rcu_head *rcu)
{
	struct vpt1_gw *gw = container_of(rcu, struct vpt1_gw, rcu);

	acct_put(gw->acct);
	atomic_long_dec(&vpt1_gw_count);
	kfree(gw);
}

static void gw_life_put(struct vpt1_gw *gw)
{
	if (refcount_dec_and_test(&gw->life))
		call_rcu(&gw->rcu, gw_free);
}

static void peer_free(struct rcu_head *rcu)
{
	struct vpt1_peer *peer = container_of(rcu, struct vpt1_peer, rcu);

	WARN_ON_ONCE(peer->frag); /* Destroyed by peer_drain(), which precedes every free. */
	kvfree(peer->errs);
	vpt1_gw_mem_release(peer->gw, peer->errs_charge);
	gw_life_put(peer->gw);
	atomic_long_dec(&vpt1_peer_count);
	kfree(peer);
}

void vpt1_peer_put(struct vpt1_peer *peer)
{
	if (peer && refcount_dec_and_test(&peer->refs))
		call_rcu(&peer->rcu, peer_free);
}

static void bucket_init(struct vpt1_bucket *b, u64 rate)
{
	b->milli = 2 * rate * 1000;
	b->last_ns = 0;
}

static bool bucket_take(struct vpt1_bucket *b, u64 rate, u64 now_ns)
{
	u64 cap = 2 * rate * 1000;

	if (b->last_ns && now_ns > b->last_ns) {
		u64 add = div_u64((now_ns - b->last_ns) * rate, 1000000ULL);

		b->milli = min(cap, b->milli + add);
	}
	if (!b->last_ns || now_ns > b->last_ns)
		b->last_ns = now_ns;
	if (b->milli < 1000)
		return false;
	b->milli -= 1000;
	return true;
}

static bool inc_in_use(struct vpt1_gw *gw, const struct vpt1_incarnation *inc)
{
	unsigned int slot;

	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *peer =
			rcu_dereference_protected(gw->peers[slot],
						  lockdep_is_held(&gw->cfg_lock));

		if (peer && !memcmp(peer->inc.id, inc->id, VPT1_ID_LEN))
			return true;
	}
	return false;
}

/* cfg_lock held. New identity, nonzero and distinct from every retained one. */
static struct vpt1_peer *peer_alloc(struct vpt1_gw *gw, unsigned int slot,
				    u64 epoch)
{
	struct vpt1_peer *peer = gw_zalloc(sizeof(*peer), GFP_KERNEL);
	static const u8 zero[VPT1_ID_LEN];

	if (!peer)
		return NULL;
	peer->errs = gw_kvzalloc(sizeof(*peer->errs));
	if (!peer->errs) {
		kfree(peer);
		return NULL;
	}
	/* The recent-error rings count against the aggregate runtime-memory budget. */
	peer->errs_charge = kmalloc_size_roundup(sizeof(*peer->errs));
	if (!vpt1_gw_mem_reserve(gw, peer->errs_charge)) {
		kvfree(peer->errs);
		kfree(peer);
		return NULL;
	}
	spin_lock_init(&peer->errs->lock);
	spin_lock_init(&peer->lock);
	refcount_set(&peer->refs, 1);
	refcount_inc(&gw->life);
	peer->gw = gw;
	peer->slot = slot;
	peer->state = VPT1_PEER_DOWN;
	peer->owner_epoch = epoch;
	atomic_set(&peer->inflight, 0);
	bucket_init(&peer->new_flows, VPT1_NEW_FLOW_RATE);
	bucket_init(&peer->errors, VPT1_ERROR_RATE);
	bucket_init(&peer->exchange, 1);
	do {
		get_random_bytes(peer->inc.id, VPT1_ID_LEN);
	} while (!memcmp(peer->inc.id, zero, VPT1_ID_LEN) ||
		 inc_in_use(gw, &peer->inc));
	atomic_long_inc(&vpt1_peer_count);
	return peer;
}

static struct vpt1_peer *peer_locked(struct vpt1_gw *gw, unsigned int slot);

/* The generations a peer held, taken out under its lock to be released outside it. */
struct vpt1_gens {
	struct vpt1_generation *g[3];
	struct vpt1_continuity *cont; /* Already marked dead. */
	struct vpt1_continuity *cont2; /* A candidate's, never used. */
};

/* Under the peer lock: forget the candidate's activation state (its generation is the caller's). */
static void peer_clear_candidate_locked(struct vpt1_peer *peer)
{
	peer->act_state = ACT_NONE;
	peer->next_fresh = false;
	WRITE_ONCE(peer->next_local_rx, 0);
}

/* Under the peer lock: take current, next and prev. */
static void peer_take_gens_locked(struct vpt1_peer *peer, struct vpt1_gens *out)
{
	out->g[0] = peer->gen;
	out->g[1] = peer->next_gen;
	out->g[2] = peer->prev_gen;
	out->cont = peer->cont;
	out->cont2 = peer->next_cont;
	if (out->cont)
		atomic_set(&out->cont->dead, 1);
	peer->cont = NULL;
	peer->next_cont = NULL;
	peer_clear_candidate_locked(peer);
	peer->act_closed = false;
	peer->grant_valid = false;
	peer->rk_state = RK_NONE;
	memzero_explicit(peer->gen_hash, sizeof(peer->gen_hash));
	memzero_explicit(peer->next_hash, sizeof(peer->next_hash));
	peer->live_armed = false;
	peer->live_lost = false;
	memset(peer->chal, 0, sizeof(peer->chal));
	peer->gen = NULL;
	peer->next_gen = NULL;
	peer->prev_gen = NULL;
	WRITE_ONCE(peer->local_rx, 0);
	WRITE_ONCE(peer->remote_rx, 0);
	WRITE_ONCE(peer->next_local_rx, 0);
	WRITE_ONCE(peer->prev_local_rx, 0);
}

/* Disables and drops each. Any context. */
static void gens_release(struct vpt1_gens *gens)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gens->g); i++) {
		if (gens->g[i]) {
			vpt1_generation_disable(gens->g[i]);
			vpt1_generation_put(gens->g[i]);
			gens->g[i] = NULL;
		}
	}
	if (gens->cont) {
		vpt1_continuity_put(gens->cont);
		gens->cont = NULL;
	}
	if (gens->cont2) {
		vpt1_continuity_put(gens->cont2);
		gens->cont2 = NULL;
	}
}

/* Marks a peer closed for good and drops its generations. Any context. */
static void peer_invalidate(struct vpt1_peer *peer)
{
	struct vpt1_gens gens;

	spin_lock_bh(&peer->lock);
	peer->state = VPT1_PEER_INVALID;
	peer_take_gens_locked(peer, &gens);
	spin_unlock_bh(&peer->lock);
	gens_release(&gens);
}

/* Continuity ----------------------------------------------------------------- */

static struct vpt1_continuity *cont_alloc(void)
{
	struct vpt1_continuity *c = gw_zalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return NULL;
	refcount_set(&c->refs, 1);
	c->id = atomic64_inc_return(&vpt1_cont_id);
	atomic_long_inc(&vpt1_cont_count);
	return c;
}

void vpt1_continuity_put(void *continuity)
{
	struct vpt1_continuity *c = continuity;

	if (c && refcount_dec_and_test(&c->refs)) {
		atomic_long_dec(&vpt1_cont_count);
		kfree(c);
	}
}

bool vpt1_continuity_try_get(void *continuity)
{
	struct vpt1_continuity *c = continuity;

	if (!c || atomic_read(&c->dead) || !refcount_inc_not_zero(&c->refs))
		return false;
	if (atomic_read(&c->dead)) { /* Killed between the check and the reference. */
		vpt1_continuity_put(c);
		return false;
	}
	return true;
}

u64 vpt1_continuity_id(const struct vpt1_continuity *continuity)
{
	return continuity->id;
}

bool vpt1_continuity_dead(const struct vpt1_continuity *continuity)
{
	return atomic_read(&continuity->dead);
}

struct vpt1_continuity *vpt1_peer_continuity_get(struct vpt1_peer *peer)
{
	struct vpt1_continuity *c;

	spin_lock_bh(&peer->lock);
	c = peer->cont;
	if (c && (atomic_read(&c->dead) || !refcount_inc_not_zero(&c->refs)))
		c = NULL;
	spin_unlock_bh(&peer->lock);
	return c;
}

/* Whole-runtime memory budget ------------------------------------------------- */

/*
 * Reserve @bytes against the host ceiling first, then this gateway's ceiling; on either failure
 * nothing is held. Lock-free and safe in any context (softirq intake, the gateway worker, or a
 * process-context commit). The caller releases exactly what it reserved. A single reservation
 * larger than the per-gateway ceiling is refused outright.
 */
bool vpt1_gw_mem_reserve(struct vpt1_gw *gw, size_t bytes)
{
	if (bytes > VPT1_GW_MEM_BUDGET)
		return false;
	if (atomic_long_add_return(bytes, &vpt1_host_mem) > (long)VPT1_HOST_MEM_BUDGET) {
		atomic_long_sub(bytes, &vpt1_host_mem);
		return false;
	}
	if (atomic_long_add_return(bytes, &gw->acct->mem) > (long)VPT1_GW_MEM_BUDGET) {
		atomic_long_sub(bytes, &gw->acct->mem);
		atomic_long_sub(bytes, &vpt1_host_mem);
		return false;
	}
	return true;
}

void vpt1_gw_mem_release(struct vpt1_gw *gw, size_t bytes)
{
	if (!bytes)
		return;
	if (WARN_ON_ONCE(atomic_long_sub_return(bytes, &gw->acct->mem) < 0))
		atomic_long_set(&gw->acct->mem, 0);
	if (WARN_ON_ONCE(atomic_long_sub_return(bytes, &vpt1_host_mem) < 0))
		atomic_long_set(&vpt1_host_mem, 0);
}

size_t vpt1_gw_mem_used(struct vpt1_gw *gw)
{
	return atomic_long_read(&gw->acct->mem);
}

size_t vpt1_host_mem_used(void)
{
	return atomic_long_read(&vpt1_host_mem);
}

/* Fragment budget ------------------------------------------------------------- */

bool vpt1_gw_frag_reserve(void *gateway, size_t charge)
{
	struct vpt1_gw *gw = gateway;

	if (charge > VPT1_FRAG_BUDGET)
		return false;
	/* The aggregate ceilings first; the per-gateway fragment sub-budget second. */
	if (!vpt1_gw_mem_reserve(gw, charge))
		return false;
	if (atomic_long_add_return(charge, &gw->acct->frag) > VPT1_FRAG_BUDGET) {
		atomic_long_sub(charge, &gw->acct->frag);
		vpt1_gw_mem_release(gw, charge);
		return false;
	}
	return true;
}

void vpt1_gw_frag_release(void *gateway, size_t charge)
{
	struct vpt1_gw *gw = gateway;

	if (WARN_ON_ONCE(atomic_long_sub_return(charge, &gw->acct->frag) < 0))
		atomic_long_set(&gw->acct->frag, 0);
	vpt1_gw_mem_release(gw, charge);
}

size_t vpt1_gw_frag_used(struct vpt1_gw *gw)
{
	return atomic_long_read(&gw->acct->frag);
}

/* Job queue ------------------------------------------------------------------- */

unsigned int vpt1_job_pending(struct vpt1_gw *gw)
{
	unsigned int n;

	spin_lock_bh(&gw->job_lock);
	n = gw->job_count;
	spin_unlock_bh(&gw->job_lock);
	return n;
}

int vpt1_job_queue(struct vpt1_peer *peer, struct vpt1_job *job)
{
	struct vpt1_gw *gw = peer->gw;
	int err = 0;

	if (!vpt1_gw_wq)
		return -ESHUTDOWN;
	spin_lock_bh(&gw->job_lock);
	/*
	 * The peer's state is read under job_lock and invalidation is followed by a cancel under
	 * job_lock, so a job is either refused here or seen by that cancel.
	 */
	if (gw->jobs_closed) {
		err = -ESHUTDOWN;
	} else if (READ_ONCE(peer->state) == VPT1_PEER_INVALID) {
		err = -ESTALE;
	} else if (gw->job_count >= VPT1_JOB_QUEUE_MAX) {
		err = -EBUSY;
	} else {
		refcount_inc(&peer->refs);
		job->peer = peer;
		list_add_tail(&job->node, &gw->jobs);
		gw->job_count++;
		queue_work(vpt1_gw_wq, &gw->jobs_work);
	}
	spin_unlock_bh(&gw->job_lock);
	return err;
}

static void jobs_work_fn(struct work_struct *work)
{
	struct vpt1_gw *gw = container_of(work, struct vpt1_gw, jobs_work);
	struct vpt1_job *job;
	struct vpt1_peer *peer;

	for (;;) {
		spin_lock_bh(&gw->job_lock);
		job = list_first_entry_or_null(&gw->jobs, struct vpt1_job, node);
		if (!job) {
			spin_unlock_bh(&gw->job_lock);
			return;
		}
		list_del_init(&job->node);
		gw->job_count--;
		peer = job->peer;
		gw->job_running = peer;
		spin_unlock_bh(&gw->job_lock);

		job->run(job, false); /* May free the job. */

		spin_lock_bh(&gw->job_lock);
		gw->job_running = NULL;
		spin_unlock_bh(&gw->job_lock);
		wake_up_var(&gw->job_running);
		vpt1_peer_put(peer);
		cond_resched();
	}
}

/* Runs the cancelled callbacks of the jobs moved to @dead, outside job_lock. */
static void jobs_cancel_list(struct list_head *dead)
{
	struct vpt1_job *job, *tmp;

	list_for_each_entry_safe(job, tmp, dead, node) {
		struct vpt1_peer *peer = job->peer;

		list_del_init(&job->node);
		job->run(job, true);
		vpt1_peer_put(peer);
	}
}

/*
 * Cancels the jobs queued for @peer (all of them when NULL) and, for a peer, waits for the one
 * running job if it is that peer's. Process context; callers hold no job or peer lock.
 */
static void jobs_cancel(struct vpt1_gw *gw, struct vpt1_peer *peer)
{
	struct vpt1_job *job, *tmp;
	LIST_HEAD(dead);

	spin_lock_bh(&gw->job_lock);
	list_for_each_entry_safe(job, tmp, &gw->jobs, node) {
		if (peer && job->peer != peer)
			continue;
		list_move_tail(&job->node, &dead);
		gw->job_count--;
	}
	spin_unlock_bh(&gw->job_lock);
	jobs_cancel_list(&dead);
	if (peer)
		wait_var_event(&gw->job_running, READ_ONCE(gw->job_running) != peer);
}

/* Destroy: refuse new jobs, cancel the queued ones, wait for the running one. */
static void jobs_close(struct vpt1_gw *gw)
{
	spin_lock_bh(&gw->job_lock);
	gw->jobs_closed = true;
	spin_unlock_bh(&gw->job_lock);
	jobs_cancel(gw, NULL);
	cancel_work_sync(&gw->jobs_work);
}

/*
 * Close and free the fragment cache. The peer is INVALID, its queued jobs are cancelled and the
 * running one has returned, so nothing else can use the cache (jobs create and use it; this runs
 * in process context under cfg_lock or on the worker). Results are freed inside the job that
 * took them.
 */
static void peer_frag_destroy(struct vpt1_peer *peer)
{
	struct vpt1_frag_cache *cache = peer->frag;

	if (!cache)
		return;
	peer->frag = NULL;
	vpt1_frag_close(cache);
	WARN_ON_ONCE(vpt1_frag_destroy(cache));
}

/* cfg_lock held. Drops the open fragment sets of a context that has just been killed. */
static void peer_frag_flush(struct vpt1_peer *peer, struct vpt1_continuity *cont)
{
	/* Pairs with the release in vpt1_peer_frag_cache(): a published cache is initialized. */
	struct vpt1_frag_cache *cache = smp_load_acquire(&peer->frag);

	if (cache && cont)
		vpt1_frag_flush_context(cache, cont);
}

static void peer_err_clear(struct vpt1_peer *peer)
{
	struct vpt1_err_table *t = peer->errs;

	spin_lock_bh(&t->lock);
	memset(t->dir, 0, sizeof(t->dir));
	spin_unlock_bh(&t->lock);
}

static void peer_drain(struct vpt1_peer *peer)
{
	/* The peer is already invalid (every caller), so no new job is accepted for it. */
	jobs_cancel(peer->gw, peer);
	/* "<= 0": an unbalanced done() is a bug (warned there), never a hang. */
	wait_var_event(&peer->inflight, atomic_read(&peer->inflight) <= 0);
	peer_frag_destroy(peer);
}

struct vpt1_peer *vpt1_gw_peer_get(struct vpt1_gw *gw, unsigned int slot)
{
	struct vpt1_peer *peer;

	if (slot < 1 || slot > VPT1_MAX_PEERS)
		return NULL;
	rcu_read_lock();
	peer = rcu_dereference(gw->peers[slot]);
	if (peer && !refcount_inc_not_zero(&peer->refs))
		peer = NULL;
	rcu_read_unlock();
	return peer;
}

void vpt1_peer_incarnation(struct vpt1_peer *peer, struct vpt1_incarnation *out)
{
	*out = peer->inc; /* Immutable after allocation. */
}

/*
 * Under the peer lock: next becomes current; the replaced current becomes prev. Returns the old
 * prev, which the caller releases after unlocking.
 */
static struct vpt1_generation *peer_promote_locked(struct vpt1_peer *peer, u64 now_ns)
{
	struct vpt1_generation *retired = peer->prev_gen;

	peer->prev_gen = peer->gen;
	WRITE_ONCE(peer->prev_local_rx, peer->gen ? peer->local_rx : 0);
	peer->prev_deadline_ns = now_ns + VPT1_PREV_GRACE_NS;
	peer->gen = peer->next_gen;
	WRITE_ONCE(peer->local_rx, peer->next_local_rx);
	WRITE_ONCE(peer->remote_rx, peer->next_remote_rx);
	peer->next_gen = NULL;
	WRITE_ONCE(peer->next_local_rx, 0);
	return retired;
}

static void peer_promoted(struct vpt1_peer *peer, struct vpt1_generation *retired)
{
	if (retired) {
		vpt1_generation_disable(retired);
		vpt1_generation_put(retired);
	}
	if (vpt1_gw_wq)
		queue_delayed_work(vpt1_gw_wq, &peer->gw->gens_work,
				   nsecs_to_jiffies(VPT1_PREV_GRACE_NS) + 1);
}

struct vpt1_generation *vpt1_peer_tx_generation_get(struct vpt1_peer *peer, u64 now_ns,
						    u64 *remote_rx)
{
	struct vpt1_generation *gen;

	spin_lock_bh(&peer->lock);
	gen = peer->gen;
	if (gen) {
		vpt1_generation_get(gen);
		*remote_rx = peer->remote_rx;
	}
	spin_unlock_bh(&peer->lock);
	return gen;
}

struct vpt1_generation *vpt1_peer_candidate_tx_get(struct vpt1_peer *peer, u64 *remote_rx)
{
	struct vpt1_generation *gen;

	spin_lock_bh(&peer->lock);
	gen = peer->next_gen;
	if (gen) {
		vpt1_generation_get(gen);
		*remote_rx = peer->next_remote_rx;
	}
	spin_unlock_bh(&peer->lock);
	return gen;
}

bool vpt1_peer_gen_is_candidate(struct vpt1_peer *peer, const struct vpt1_generation *gen)
{
	bool cand;

	spin_lock_bh(&peer->lock);
	cand = gen && gen == peer->next_gen;
	spin_unlock_bh(&peer->lock);
	return cand;
}

struct vpt1_generation *vpt1_peer_rx_generation_get(struct vpt1_peer *peer, u64 locator,
						    u64 now_ns)
{
	struct vpt1_generation *gen = NULL;

	if (!locator)
		return NULL;
	spin_lock_bh(&peer->lock);
	if (peer->gen && peer->local_rx == locator)
		gen = peer->gen;
	else if (peer->next_gen && peer->next_local_rx == locator)
		gen = peer->next_gen;
	else if (peer->prev_gen && peer->prev_local_rx == locator && now_ns < peer->prev_deadline_ns)
		gen = peer->prev_gen;
	if (gen)
		vpt1_generation_get(gen);
	spin_unlock_bh(&peer->lock);
	return gen;
}

/* Liveness -------------------------------------------------------------------- */

/* Peer lock held. A commit that never got COMMITTED closes the peer outright. */
static bool live_closed_locked(const struct vpt1_peer *peer, u64 now_ns)
{
	if (peer->act_closed)
		return true;
	return peer->live_armed && now_ns > peer->live_last_ns &&
	       now_ns - peer->live_last_ns >= VPT1_LIVE_NS;
}

/* Peer lock held: a new session starts the clock; the first PING follows shortly. */
static void live_arm_locked(struct vpt1_peer *peer, u64 now_ns)
{
	peer->act_closed = false;
	peer->live_armed = true;
	peer->live_lost = false;
	peer->live_last_ns = now_ns;
	peer->ping_due_ns = now_ns + VPT1_PING_FIRST_NS;
	memset(peer->chal, 0, sizeof(peer->chal));
}

bool vpt1_peer_live_closed(struct vpt1_peer *peer, u64 now_ns)
{
	bool closed;

	spin_lock_bh(&peer->lock);
	closed = live_closed_locked(peer, now_ns);
	spin_unlock_bh(&peer->lock);
	return closed;
}

bool vpt1_peer_live_challenge(struct vpt1_peer *peer, u64 now_ns, u8 out[VPT1_CHALLENGE_LEN])
{
	unsigned int i;
	bool made = false;

	spin_lock_bh(&peer->lock);
	if (peer->live_armed && peer->state == VPT1_PEER_READY && peer->gen &&
	    !live_closed_locked(peer, now_ns) && now_ns >= peer->ping_due_ns) {
		for (i = 0; i < VPT1_CHALLENGES; i++) {
			if (peer->chal[i].expiry_ns > now_ns)
				continue; /* Still answerable. */
			get_random_bytes(peer->chal[i].c, VPT1_CHALLENGE_LEN);
			peer->chal[i].expiry_ns = now_ns + VPT1_CHALLENGE_NS;
			memcpy(out, peer->chal[i].c, VPT1_CHALLENGE_LEN);
			peer->ping_due_ns = now_ns + VPT1_PING_NS;
			made = true;
			break;
		}
	}
	spin_unlock_bh(&peer->lock);
	return made;
}

bool vpt1_peer_live_pong(struct vpt1_peer *peer, const u8 c[VPT1_CHALLENGE_LEN], u64 now_ns)
{
	unsigned int i;
	bool matched = false;

	spin_lock_bh(&peer->lock);
	/* A closed peer needs a new session; a late answer does not reopen it. */
	if (peer->live_armed && !live_closed_locked(peer, now_ns)) {
		for (i = 0; i < VPT1_CHALLENGES; i++) {
			if (peer->chal[i].expiry_ns > now_ns &&
			    !memcmp(peer->chal[i].c, c, VPT1_CHALLENGE_LEN)) {
				peer->chal[i].expiry_ns = 0; /* Consumed: it cannot refresh health again. */
				memzero_explicit(peer->chal[i].c, VPT1_CHALLENGE_LEN);
				if (now_ns > peer->live_last_ns)
					peer->live_last_ns = now_ns;
				matched = true;
				break;
			}
		}
	}
	spin_unlock_bh(&peer->lock);
	return matched;
}

bool vpt1_peer_gen_is_current(struct vpt1_peer *peer, const struct vpt1_generation *gen)
{
	bool current_gen;

	spin_lock_bh(&peer->lock);
	current_gen = gen && gen == peer->gen;
	spin_unlock_bh(&peer->lock);
	return current_gen;
}

bool vpt1_gw_lease_live(struct vpt1_gw *gw, u64 now_ns)
{
	return !READ_ONCE(gw->stopping) && READ_ONCE(gw->live_epoch) &&
	       now_ns < READ_ONCE(gw->deadline_ns);
}

/* Activation and granted rekey (TECHNICAL_DESIGN sections 5.2 and 6) ----------------------- */

/* CONFIRM / COMMIT / REKEY_START retries: 0.5, 1, 2, then every 4 s, inside the attempt deadline. */
static u64 act_interval(unsigned int step)
{
	static const unsigned int ms[] = { 500, 1000, 2000, 4000 };

	return (u64)ms[min_t(unsigned int, step, ARRAY_SIZE(ms) - 1)] * NSEC_PER_MSEC;
}

static void act_schedule(struct vpt1_gw *gw, u64 delay_ns)
{
	if (vpt1_gw_wq && !READ_ONCE(gw->stopping))
		mod_delayed_work(vpt1_gw_wq, &gw->act_work, nsecs_to_jiffies(delay_ns));
}

/*
 * Peer lock held, a candidate present. It becomes current. A fresh candidate replaces the whole
 * session: the old keys and the old continuity go (flows end with them) and the liveness clock is
 * re-armed. A continuity-keeping one retires the old current into prev and keeps both continuity
 * and clock; the commit itself is the round-trip proof that refreshes the clock.
 */
static void peer_commit_locked(struct vpt1_peer *peer, u64 now_ns, struct vpt1_gens *gens,
			       struct vpt1_generation **retired)
{
	bool fresh = peer->next_fresh;

	if (fresh) {
		gens->g[0] = peer->gen;
		gens->g[2] = peer->prev_gen;
		gens->cont = peer->cont;
		if (gens->cont)
			atomic_set(&gens->cont->dead, 1);
		peer->gen = NULL;
		peer->prev_gen = NULL;
		peer->cont = peer->next_cont;
		peer->next_cont = NULL;
	}
	*retired = peer_promote_locked(peer, now_ns);
	memcpy(peer->gen_hash, peer->next_hash, VPT1_HASH_LEN);
	memcpy(peer->gen_att, peer->next_att, VPT1_ATTEMPT_ID_LEN);
	peer->next_fresh = false;
	peer->act_state = ACT_NONE;
	peer->rk_state = RK_NONE;
	peer->grant_valid = false;
	if (fresh)
		live_arm_locked(peer, now_ns);
	else if (now_ns > peer->live_last_ns)
		peer->live_last_ns = now_ns;
}

static void peer_committed(struct vpt1_peer *peer, struct vpt1_gens *gens,
			   struct vpt1_generation *retired)
{
	/*
	 * A fresh session ended the old continuity: its error contexts go. Its open fragment sets
	 * cannot complete (the continuity is dead) and age out; flushing needs a sleeping lock and
	 * this can run in softirq.
	 */
	if (gens->cont)
		peer_err_clear(peer);
	gens_release(gens);
	peer_promoted(peer, retired);
	ctr_inc(peer->gw, VPT1_E_ACTIVATED);
	if (vpt1_gw_wq && !READ_ONCE(peer->gw->stopping))
		mod_delayed_work(vpt1_gw_wq, &peer->gw->live_work, HZ);
}

bool vpt1_peer_commit(struct vpt1_peer *peer, u64 now_ns)
{
	struct vpt1_generation *retired = NULL;
	struct vpt1_gens gens = {};
	bool done = false;

	spin_lock_bh(&peer->lock);
	if (peer->next_gen && peer->state == VPT1_PEER_READY) {
		peer_commit_locked(peer, now_ns, &gens, &retired);
		done = true;
	}
	spin_unlock_bh(&peer->lock);
	if (done)
		peer_committed(peer, &gens, retired);
	return done;
}

static void out_hash(struct vpt1_ctl_out *out, u8 subtype, enum vpt1_send_gen which, const u8 *hash)
{
	out->subtype = subtype;
	out->which = which;
	memcpy(out->payload, hash, VPT1_HASH_LEN);
	out->len = VPT1_HASH_LEN;
}

int vpt1_peer_control_rx(struct vpt1_peer *peer, struct vpt1_generation *gen, u8 subtype,
			 const u8 *payload, unsigned int len, u64 now_ns, struct vpt1_ctl_out *out)
{
	struct vpt1_generation *retired = NULL;
	struct vpt1_gens gens = {};
	bool committed = false, grant = false, needed = false;
	int err = -EPROTO;
	bool cand, cur;

	memset(out, 0, sizeof(*out));
	spin_lock_bh(&peer->lock);
	if (peer->state != VPT1_PEER_READY || !gen)
		goto unlock;
	cand = gen == peer->next_gen;
	cur = gen == peer->gen && !live_closed_locked(peer, now_ns);
	switch (subtype) {
	case VPT1_CONFIRM: /* Responder: answer under the candidate (or, repeated, the current). */
		if (len != VPT1_HASH_LEN || peer->role)
			break;
		if (cand && now_ns < peer->act_deadline_ns &&
		    !memcmp(payload, peer->next_hash, VPT1_HASH_LEN)) {
			out_hash(out, VPT1_READY, VPT1_SEND_CANDIDATE, payload);
			err = 0;
		} else if (cur && !memcmp(payload, peer->gen_hash, VPT1_HASH_LEN)) {
			out_hash(out, VPT1_READY, VPT1_SEND_CURRENT, payload);
			err = 0;
		}
		break;
	case VPT1_READY: /* Initiator: commit locally, then COMMIT. */
		if (len != VPT1_HASH_LEN || !peer->role)
			break;
		if (cand && peer->act_state == ACT_WAIT_READY && now_ns < peer->act_deadline_ns &&
		    !memcmp(payload, peer->next_hash, VPT1_HASH_LEN)) {
			peer_commit_locked(peer, now_ns, &gens, &retired);
			peer->act_state = ACT_WAIT_COMMITTED;
			peer->act_step = 0;
			peer->act_due_ns = now_ns + act_interval(0);
			peer->act_deadline_ns = now_ns + VPT1_ATTEMPT_NS;
			committed = true;
			out_hash(out, VPT1_COMMIT, VPT1_SEND_CURRENT, payload);
			err = 0;
		} else if (cur && peer->act_state == ACT_WAIT_COMMITTED &&
			   !memcmp(payload, peer->gen_hash, VPT1_HASH_LEN)) {
			out_hash(out, VPT1_COMMIT, VPT1_SEND_CURRENT, payload);
			err = 0;
		}
		break;
	case VPT1_COMMIT: /* Responder: commit locally, then COMMITTED (and again for a repeat). */
		if (len != VPT1_HASH_LEN || peer->role)
			break;
		if (cand && now_ns < peer->act_deadline_ns &&
		    !memcmp(payload, peer->next_hash, VPT1_HASH_LEN)) {
			peer_commit_locked(peer, now_ns, &gens, &retired);
			committed = true;
			out_hash(out, VPT1_COMMITTED, VPT1_SEND_CURRENT, payload);
			err = 0;
		} else if (cur && !memcmp(payload, peer->gen_hash, VPT1_HASH_LEN)) {
			out_hash(out, VPT1_COMMITTED, VPT1_SEND_CURRENT, payload);
			err = 0;
		}
		break;
	case VPT1_COMMITTED: /* Initiator: the activation is complete. */
		if (len != VPT1_HASH_LEN || !peer->role || !cur ||
		    memcmp(payload, peer->gen_hash, VPT1_HASH_LEN))
			break;
		if (peer->act_state == ACT_WAIT_COMMITTED) {
			peer->act_state = ACT_NONE;
			if (now_ns > peer->live_last_ns)
				peer->live_last_ns = now_ns;
		}
		err = 0;
		break;
	case VPT1_REKEY_START: /* Responder: one grant, bound to its current generation. */
		if (len != VPT1_REKEY_PAYLOAD_LEN || peer->role || !cur ||
		    memcmp(payload + VPT1_ATTEMPT_ID_LEN, peer->gen_hash, VPT1_HASH_LEN))
			break;
		if (peer->grant_valid && !memcmp(peer->grant_att, payload, VPT1_ATTEMPT_ID_LEN)) {
			/* A repeat never extends the grant; it is answered while the grant lasts. */
			if (!peer->grant_used && now_ns < peer->grant_deadline_ns) {
				memcpy(out->payload, payload, VPT1_REKEY_PAYLOAD_LEN);
				out->subtype = VPT1_REKEY_OK;
				out->len = VPT1_REKEY_PAYLOAD_LEN;
				out->which = VPT1_SEND_CURRENT;
				err = 0;
			}
			break;
		}
		if (peer->grant_valid && !peer->grant_used && now_ns < peer->grant_deadline_ns)
			break; /* One live grant at a time. */
		peer->grant_valid = true;
		peer->grant_used = false;
		memcpy(peer->grant_att, payload, VPT1_ATTEMPT_ID_LEN);
		memcpy(peer->grant_parent, payload + VPT1_ATTEMPT_ID_LEN, VPT1_HASH_LEN);
		peer->grant_deadline_ns = now_ns + VPT1_ATTEMPT_NS;
		memcpy(out->payload, payload, VPT1_REKEY_PAYLOAD_LEN);
		out->subtype = VPT1_REKEY_OK;
		out->len = VPT1_REKEY_PAYLOAD_LEN;
		out->which = VPT1_SEND_CURRENT;
		grant = true;
		err = 0;
		break;
	case VPT1_REKEY_OK: /* Initiator: the grant for the attempt it asked about. */
		if (len != VPT1_REKEY_PAYLOAD_LEN || !peer->role || !cur ||
		    peer->rk_state == RK_NONE || now_ns >= peer->rk_deadline_ns ||
		    memcmp(payload, peer->rk_att, VPT1_ATTEMPT_ID_LEN) ||
		    memcmp(payload + VPT1_ATTEMPT_ID_LEN, peer->rk_parent, VPT1_HASH_LEN))
			break;
		if (peer->rk_state == RK_REQUESTED) {
			peer->rk_state = RK_GRANTED;
			out->event = VPT1_EVT_REKEY_GRANTED;
			memcpy(out->attempt, peer->rk_att, VPT1_ATTEMPT_ID_LEN);
			grant = true;
		}
		err = 0;
		break;
	case VPT1_REKEY_NEEDED: /* Initiator: the responder's keys are near their limits. */
		if (len != VPT1_HASH_LEN || !peer->role || !cur ||
		    memcmp(payload, peer->gen_hash, VPT1_HASH_LEN))
			break;
		if (now_ns >= peer->needed_ns + VPT1_REKEY_NEEDED_NS || !peer->needed_ns) {
			peer->needed_ns = now_ns;
			out->event = VPT1_EVT_REKEY_NEEDED;
			needed = true;
		}
		err = 0;
		break;
	default:
		break;
	}
unlock:
	spin_unlock_bh(&peer->lock);
	if (committed)
		peer_committed(peer, &gens, retired);
	if (grant)
		ctr_inc(peer->gw, VPT1_E_REKEY_GRANTS);
	if (needed)
		ctr_inc(peer->gw, VPT1_E_REKEY_NEEDED);
	if (!err && subtype >= VPT1_CONFIRM && subtype <= VPT1_COMMITTED)
		ctr_inc(peer->gw, VPT1_E_RX_ACTIVATION);
	if (committed && peer->role)
		act_schedule(peer->gw, act_interval(0));
	return err;
}

/* Candidate expiry, CONFIRM / COMMIT / REKEY_START retries, and the failed-commit closure. */
static void act_work_fn(struct work_struct *work)
{
	struct vpt1_gw *gw = container_of(to_delayed_work(work), struct vpt1_gw, act_work);
	u64 now = ktime_get_boottime_ns(), soonest = 0;
	unsigned int slot;

	if (!vpt1_gw_get(gw))
		return;
	mutex_lock(&gw->cfg_lock);
	for (slot = 1; slot <= VPT1_MAX_PEERS && !gw->stopping; slot++) {
		struct vpt1_peer *peer = peer_locked(gw, slot);
		u8 payload[VPT1_REKEY_PAYLOAD_LEN];
		enum vpt1_send_gen which = VPT1_SEND_CURRENT;
		struct vpt1_gens gens = {};
		unsigned int plen = 0;
		bool failed = false;
		u8 sub = 0;

		if (!peer)
			continue;
		spin_lock_bh(&peer->lock);
		if (peer->next_gen && now >= peer->next_deadline_ns) {
			gens.g[1] = peer->next_gen;
			gens.cont2 = peer->next_cont;
			peer->next_gen = NULL;
			peer->next_cont = NULL;
			peer_clear_candidate_locked(peer);
			failed = true;
		} else if (peer->next_gen && peer->act_state == ACT_WAIT_READY && now >= peer->act_due_ns) {
			sub = VPT1_CONFIRM;
			which = VPT1_SEND_CANDIDATE;
			memcpy(payload, peer->next_hash, VPT1_HASH_LEN);
			plen = VPT1_HASH_LEN;
			peer->act_due_ns = now + act_interval(++peer->act_step);
		} else if (peer->gen && peer->act_state == ACT_WAIT_COMMITTED) {
			if (now >= peer->act_deadline_ns) {
				/* Committed locally, never confirmed: no way back to the old key. */
				peer->act_state = ACT_NONE;
				peer->act_closed = true;
				if (peer->cont)
					atomic_set(&peer->cont->dead, 1);
				failed = true;
			} else if (now >= peer->act_due_ns) {
				sub = VPT1_COMMIT;
				memcpy(payload, peer->gen_hash, VPT1_HASH_LEN);
				plen = VPT1_HASH_LEN;
				peer->act_due_ns = now + act_interval(++peer->act_step);
			}
		}
		if (!sub && peer->rk_state == RK_REQUESTED && peer->gen) {
			if (now >= peer->rk_deadline_ns) {
				peer->rk_state = RK_NONE;
			} else if (now >= peer->rk_due_ns) {
				sub = VPT1_REKEY_START;
				memcpy(payload, peer->rk_att, VPT1_ATTEMPT_ID_LEN);
				memcpy(payload + VPT1_ATTEMPT_ID_LEN, peer->rk_parent, VPT1_HASH_LEN);
				plen = VPT1_REKEY_PAYLOAD_LEN;
				peer->rk_due_ns = now + act_interval(++peer->rk_step);
			}
		}
		if (peer->next_gen && (!soonest || peer->next_deadline_ns < soonest))
			soonest = peer->next_deadline_ns;
		if (peer->next_gen && peer->act_state == ACT_WAIT_READY &&
		    (!soonest || peer->act_due_ns < soonest))
			soonest = peer->act_due_ns;
		if (peer->gen && peer->act_state == ACT_WAIT_COMMITTED) {
			u64 t = min(peer->act_due_ns, peer->act_deadline_ns);

			if (!soonest || t < soonest)
				soonest = t;
		}
		if (peer->rk_state == RK_REQUESTED) {
			u64 t = min(peer->rk_due_ns, peer->rk_deadline_ns);

			if (!soonest || t < soonest)
				soonest = t;
		}
		spin_unlock_bh(&peer->lock);
		gens_release(&gens);
		if (failed)
			ctr_inc(gw, VPT1_E_ACTIVATION_FAILED);
		if (sub && vpt1_gw_lease_live(gw, now) && vpt1_gw_ops_ptr &&
		    vpt1_gw_ops_ptr->send_control)
			vpt1_gw_ops_ptr->send_control(gw, peer, which, sub, payload, plen);
	}
	mutex_unlock(&gw->cfg_lock);
	if (soonest)
		act_schedule(gw, soonest > now ? soonest - now : NSEC_PER_MSEC);
	vpt1_gw_put(gw);
}

/* Once a second: PING each live relationship when due, and act on a peer that has gone silent. */
static void live_work_fn(struct work_struct *work)
{
	struct vpt1_gw *gw = container_of(to_delayed_work(work), struct vpt1_gw, live_work);
	u64 now = ktime_get_boottime_ns();
	bool again = false;
	unsigned int slot;

	if (!vpt1_gw_get(gw))
		return;
	mutex_lock(&gw->cfg_lock);
	for (slot = 1; slot <= VPT1_MAX_PEERS && !gw->stopping; slot++) {
		struct vpt1_peer *peer = peer_locked(gw, slot);
		u8 challenge[VPT1_CHALLENGE_LEN];
		bool first_loss = false, armed, soft = false;
		u8 parent[VPT1_HASH_LEN];

		if (!peer)
			continue;
		spin_lock_bh(&peer->lock);
		armed = peer->live_armed && peer->state == VPT1_PEER_READY;
		if (armed && live_closed_locked(peer, now) && !peer->live_lost) {
			/* Flow continuity ends with the session: nothing may complete under it. */
			peer->live_lost = true;
			first_loss = true;
			if (peer->cont)
				atomic_set(&peer->cont->dead, 1);
		}
		/* Keys near their soft limits ask for a re-key (rate limited), never extend themselves. */
		if (armed && peer->gen && !live_closed_locked(peer, now) &&
		    vpt1_generation_limit(peer->gen, now) != VPT1_LIMIT_OK &&
		    (!peer->needed_ns || now >= peer->needed_ns + VPT1_REKEY_NEEDED_NS)) {
			peer->needed_ns = now;
			memcpy(parent, peer->gen_hash, VPT1_HASH_LEN);
			soft = true;
		}
		spin_unlock_bh(&peer->lock);
		if (first_loss)
			ctr_inc(gw, VPT1_E_LIVENESS_LOST);
		if (!armed)
			continue;
		again = true;
		if (!vpt1_gw_ops_ptr)
			continue;
		/*
		 * The initiator coordinates every re-key, so it tells its own controller (after a loss too:
		 * recovery is a fresh session); the responder asks the initiator with REKEY_NEEDED.
		 */
		if ((first_loss || soft) && peer->role && vpt1_gw_ops_ptr->control_event) {
			ctr_inc(gw, VPT1_E_REKEY_NEEDED);
			vpt1_gw_ops_ptr->control_event(gw, peer, VPT1_EVT_REKEY_NEEDED, NULL);
		} else if (soft && vpt1_gw_lease_live(gw, now) && vpt1_gw_ops_ptr->send_control) {
			ctr_inc(gw, VPT1_E_REKEY_NEEDED);
			vpt1_gw_ops_ptr->send_control(gw, peer, VPT1_SEND_CURRENT, VPT1_REKEY_NEEDED,
						      parent, VPT1_HASH_LEN);
		}
		if (vpt1_gw_lease_live(gw, now) && vpt1_gw_ops_ptr->send_control &&
		    vpt1_peer_live_challenge(peer, now, challenge))
			vpt1_gw_ops_ptr->send_control(gw, peer, VPT1_SEND_CURRENT, VPT1_PING, challenge,
						      VPT1_CHALLENGE_LEN);
	}
	mutex_unlock(&gw->cfg_lock);
	if (again && vpt1_gw_wq && !READ_ONCE(gw->stopping))
		queue_delayed_work(vpt1_gw_wq, &gw->live_work, HZ);
	vpt1_gw_put(gw);
}

/* Releases retired generations whose grace has run out. */
static void gens_work_fn(struct work_struct *work)
{
	struct vpt1_gw *gw = container_of(to_delayed_work(work), struct vpt1_gw, gens_work);
	u64 now = ktime_get_boottime_ns(), soonest = 0;
	unsigned int slot;

	if (!vpt1_gw_get(gw))
		return;
	mutex_lock(&gw->cfg_lock);
	for (slot = 1; slot <= VPT1_MAX_PEERS && !gw->stopping; slot++) {
		struct vpt1_peer *peer = peer_locked(gw, slot);
		struct vpt1_gens gens = {};

		if (!peer)
			continue;
		spin_lock_bh(&peer->lock);
		if (peer->prev_gen && now >= peer->prev_deadline_ns) {
			gens.g[2] = peer->prev_gen;
			peer->prev_gen = NULL;
			WRITE_ONCE(peer->prev_local_rx, 0);
		} else if (peer->prev_gen && (!soonest || peer->prev_deadline_ns < soonest)) {
			soonest = peer->prev_deadline_ns;
		}
		spin_unlock_bh(&peer->lock);
		gens_release(&gens);
	}
	mutex_unlock(&gw->cfg_lock);
	if (soonest && vpt1_gw_wq)
		queue_delayed_work(vpt1_gw_wq, &gw->gens_work, nsecs_to_jiffies(soonest - now) + 1);
	vpt1_gw_put(gw);
}

/* Gateway lifetime ------------------------------------------------------------ */

static void expiry_work_fn(struct work_struct *work);
static void gens_work_fn(struct work_struct *work);
static void live_work_fn(struct work_struct *work);
static void frag_work_fn(struct work_struct *work);
static void act_work_fn(struct work_struct *work);

struct vpt1_gw *vpt1_gw_create(u32 ifindex, u64 now_ns)
{
	struct vpt1_gw *gw;

	if (!vpt1_gw_wq || !ifindex)
		return NULL;
	gw = gw_zalloc(sizeof(*gw), GFP_KERNEL);
	if (!gw)
		return NULL;
	gw->acct = acct_alloc();
	if (!gw->acct) {
		kfree(gw);
		return NULL;
	}
	refcount_set(&gw->life, 1);
	refcount_set(&gw->users, 1);
	mutex_init(&gw->cfg_lock);
	spin_lock_init(&gw->lease_lock);
	spin_lock_init(&gw->job_lock);
	INIT_LIST_HEAD(&gw->jobs);
	INIT_WORK(&gw->jobs_work, jobs_work_fn);
	INIT_DELAYED_WORK(&gw->expiry_work, expiry_work_fn);
	INIT_DELAYED_WORK(&gw->gens_work, gens_work_fn);
	INIT_DELAYED_WORK(&gw->live_work, live_work_fn);
	INIT_DELAYED_WORK(&gw->frag_work, frag_work_fn);
	INIT_DELAYED_WORK(&gw->act_work, act_work_fn);
	gw->ifindex = ifindex;
	gw->next_epoch = 0;
	atomic_long_inc(&vpt1_gw_count);
	spin_lock(&vpt1_gw_list_lock);
	list_add(&gw->node, &vpt1_gw_list);
	spin_unlock(&vpt1_gw_list_lock);
	return gw;
}

void vpt1_gw_lock(struct vpt1_gw *gw)
{
	mutex_lock(&gw->cfg_lock);
}

void vpt1_gw_unlock(struct vpt1_gw *gw)
{
	mutex_unlock(&gw->cfg_lock);
}

u32 vpt1_gw_ifindex(const struct vpt1_gw *gw)
{
	return gw->ifindex;
}

bool vpt1_gw_get(struct vpt1_gw *gw)
{
	if (!refcount_inc_not_zero(&gw->users))
		return false;
	/* Pairs with the stopping store in close(): inc, then check. */
	smp_mb__after_atomic();
	if (READ_ONCE(gw->stopping)) {
		vpt1_gw_put(gw);
		return false;
	}
	return true;
}

void vpt1_gw_put(struct vpt1_gw *gw)
{
	/* Only the last put after destroy() reaches zero; the creator holds one. */
	if (refcount_dec_and_test(&gw->users))
		return;
	wake_up_var(&gw->users);
}

/* Lease ------------------------------------------------------------------------ */

/* Under cfg_lock. Peers, staged and cfg left by an older owner are closed. */
static void cleanup_stale(struct vpt1_gw *gw)
{
	unsigned int slot;
	u64 live;

	lockdep_assert_held(&gw->cfg_lock);
	spin_lock_bh(&gw->lease_lock);
	live = gw->live_epoch;
	spin_unlock_bh(&gw->lease_lock);

	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *peer = rcu_dereference_protected(
			gw->peers[slot], lockdep_is_held(&gw->cfg_lock));

		if (peer && peer->owner_epoch != live) {
			peer_invalidate(peer);
			RCU_INIT_POINTER(gw->peers[slot], NULL);
			peer_drain(peer);
			vpt1_peer_put(peer);
		}
	}
	if (gw->staged && gw->staged_epoch != live) {
		cfg_discard(gw->staged);
		gw->staged = NULL;
	}
	{
		struct vpt1_cfg *cfg = rcu_dereference_protected(
			gw->cfg, lockdep_is_held(&gw->cfg_lock));

		if (cfg && cfg->owner_epoch != live) {
			RCU_INIT_POINTER(gw->cfg, NULL);
			cfg_retire(cfg);
		}
	}
}

static void schedule_expiry(struct vpt1_gw *gw)
{
	if (vpt1_gw_wq)
		queue_delayed_work(vpt1_gw_wq, &gw->expiry_work,
				   nsecs_to_jiffies(VPT1_LEASE_NS) + 1);
}

int vpt1_gw_claim(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN], u32 portid,
		  u64 now_ns, u64 *epoch)
{
	bool fresh = false;
	int err = 0;

	lockdep_assert_held(&gw->cfg_lock);
	spin_lock_bh(&gw->lease_lock);
	if (gw->stopping) {
		err = -ENODEV;
	} else if (gw->live_epoch && now_ns < gw->deadline_ns) {
		/* Live lease: only the same owner renews; reuse of a portid or pid
		 * is never continuity.
		 */
		if (!memcmp(gw->controller_id, id, VPT1_ID_LEN) &&
		    gw->portid == portid) {
			WRITE_ONCE(gw->deadline_ns, now_ns + VPT1_LEASE_NS);
			*epoch = gw->live_epoch;
			ctr_inc(gw, VPT1_E_RENEWALS);
		} else {
			err = -EBUSY;
		}
	} else {
		if (gw->live_epoch)
			ctr_inc(gw, VPT1_E_LEASE_EXPIRED);
		/* New owner epoch, even for the same controller ID and portid. */
		gw->next_epoch++;
		memcpy(gw->controller_id, id, VPT1_ID_LEN);
		gw->portid = portid;
		WRITE_ONCE(gw->deadline_ns, now_ns + VPT1_LEASE_NS);
		WRITE_ONCE(gw->live_epoch, gw->next_epoch);
		*epoch = gw->next_epoch;
		fresh = true;
		ctr_inc(gw, VPT1_E_CLAIMS);
	}
	spin_unlock_bh(&gw->lease_lock);
	if (err)
		return err;
	if (fresh)
		cleanup_stale(gw);
	schedule_expiry(gw);
	return 0;
}

/* Under lease_lock. */
static int owner_check_locked(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN],
			      u32 portid, u64 now_ns)
{
	if (gw->stopping)
		return -ENODEV;
	if (!gw->live_epoch || memcmp(gw->controller_id, id, VPT1_ID_LEN) ||
	    gw->portid != portid)
		return -EPERM;
	if (now_ns >= gw->deadline_ns)
		return -ESTALE;
	return 0;
}

int vpt1_gw_check_owner(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN], u32 portid,
			u64 now_ns)
{
	int err;

	spin_lock_bh(&gw->lease_lock);
	err = owner_check_locked(gw, id, portid, now_ns);
	spin_unlock_bh(&gw->lease_lock);
	return err;
}

int vpt1_gw_renew(struct vpt1_gw *gw, const u8 id[VPT1_ID_LEN], u32 portid,
		  u64 revision, u64 now_ns)
{
	struct vpt1_cfg *cfg;
	int err;

	lockdep_assert_held(&gw->cfg_lock);
	cfg = rcu_dereference_protected(gw->cfg, lockdep_is_held(&gw->cfg_lock));
	spin_lock_bh(&gw->lease_lock);
	/* Owner, a live epoch and now < deadline, all under one lock: a renewal
	 * at or after the deadline never reopens what expiry closed.
	 */
	err = owner_check_locked(gw, id, portid, now_ns);
	if (!err && revision != (cfg ? cfg->revision : 0))
		err = -ESTALE;
	if (!err) {
		WRITE_ONCE(gw->deadline_ns, now_ns + VPT1_LEASE_NS);
		ctr_inc(gw, VPT1_E_RENEWALS);
	}
	spin_unlock_bh(&gw->lease_lock);
	if (!err)
		schedule_expiry(gw);
	else
		ctr_inc(gw, VPT1_E_REJECTED);
	return err;
}

#ifdef VPT1_DEBUG
static void dbg_stall(void)
{
	while (atomic_read(&vpt1_dbg_stall_expiry))
		msleep(20);
}
#else
static void dbg_stall(void)
{
}
#endif

void vpt1_gw_expire_check(struct vpt1_gw *gw, u64 now_ns)
{
	bool expired = false;
	u64 remaining = 0;

	dbg_stall();
	mutex_lock(&gw->cfg_lock);
	if (gw->stopping) {
		mutex_unlock(&gw->cfg_lock);
		return;
	}
	spin_lock_bh(&gw->lease_lock);
	if (gw->live_epoch) {
		if (now_ns >= gw->deadline_ns) {
			WRITE_ONCE(gw->live_epoch, 0);
			ctr_inc(gw, VPT1_E_LEASE_EXPIRED);
			expired = true;
		} else {
			remaining = gw->deadline_ns - now_ns;
		}
	} else {
		/*
		 * No live owner: either the lease ran out earlier, or the owner was released and the
		 * loss record could not be allocated (vpt1_gw_owner_released()), so nothing closed its
		 * configuration and peers. Nothing can be admitted without an owner; free them now
		 * instead of waiting for the next claim. A no-op when nothing is left.
		 */
		expired = true;
	}
	spin_unlock_bh(&gw->lease_lock);
	if (expired)
		cleanup_stale(gw);
	mutex_unlock(&gw->cfg_lock);
	if (remaining && vpt1_gw_wq)
		queue_delayed_work(vpt1_gw_wq, &gw->expiry_work,
				   nsecs_to_jiffies(remaining) + 1);
}

static void expiry_work_fn(struct work_struct *work)
{
	struct vpt1_gw *gw =
		container_of(to_delayed_work(work), struct vpt1_gw, expiry_work);

	if (!vpt1_gw_get(gw))
		return;
	vpt1_gw_expire_check(gw, ktime_get_boottime_ns());
	vpt1_gw_put(gw);
}

static void loss_work_fn(struct work_struct *work)
{
	struct vpt1_loss *loss = container_of(work, struct vpt1_loss, work);
	struct vpt1_gw *gw = loss->gw;

	dbg_stall();
	mutex_lock(&gw->cfg_lock);
	/*
	 * Recheck under the lock: a replacement owner has a different epoch, and
	 * cleanup_stale() only closes state that does not belong to the live one,
	 * so a reused portid cannot make this close the new owner.
	 */
	if (!gw->stopping)
		cleanup_stale(gw);
	mutex_unlock(&gw->cfg_lock);
	vpt1_gw_put(gw);
	kfree(loss);
}

bool vpt1_gw_owner_released(struct vpt1_gw *gw, u32 portid)
{
	struct vpt1_loss *loss;
	bool lost = false;
	u64 epoch = 0;

	spin_lock_bh(&gw->lease_lock);
	if (!gw->stopping && gw->live_epoch && gw->portid == portid) {
		epoch = gw->live_epoch;
		WRITE_ONCE(gw->live_epoch, 0);
		lost = true;
	}
	spin_unlock_bh(&gw->lease_lock);
	if (!lost)
		return false;
	ctr_inc(gw, VPT1_E_OWNER_LOST);
	loss = gw_zalloc(sizeof(*loss), GFP_ATOMIC);
	if (!loss)
		return true; /* Admission is already closed; expiry cleans up. */
	if (!vpt1_gw_get(gw)) {
		kfree(loss);
		return true;
	}
	INIT_WORK(&loss->work, loss_work_fn);
	loss->gw = gw;
	loss->epoch = epoch;
	queue_work(vpt1_gw_wq, &loss->work);
	return true;
}

/* Configuration ------------------------------------------------------------- */

static bool prefix_equal(const struct vpt1_acl_prefix *a,
			 const struct vpt1_acl_prefix *b)
{
	return a->address == b->address && a->length == b->length;
}

static bool peer_cfg_equal(const struct vpt1_cfg_peer *a,
			   const struct vpt1_cfg_peer *b)
{
	unsigned int i;

	if (a->present != b->present || a->slot != b->slot || a->role != b->role ||
	    a->pmtu != b->pmtu || a->endpoint != b->endpoint || a->port != b->port ||
	    memcmp(a->key, b->key, VPT1_KEY_BYTES) || a->owned.count != b->owned.count)
		return false;
	for (i = 0; i < a->owned.count; i++)
		if (!prefix_equal(&a->owned.prefixes[i], &b->owned.prefixes[i]))
			return false;
	return true;
}

static bool global_equal(const struct vpt1_cfg *cfg, const struct vpt1_cfg *other)
{
	return cfg->lan_ifindex == other->lan_ifindex &&
	       cfg->underlay_ifindex == other->underlay_ifindex &&
	       cfg->underlay_address == other->underlay_address &&
	       !memcmp(cfg->local_key, other->local_key, VPT1_KEY_BYTES) &&
	       prefix_equal(&cfg->lan, &other->lan) &&
	       prefix_equal(&cfg->underlay, &other->underlay) &&
	       cfg->lan_address == other->lan_address;
}

static int snapshot_check(const struct vpt1_snapshot *s)
{
	unsigned int i, j;
	unsigned int present = 0;

	if (!s->lan_ifindex || !s->underlay_ifindex || !s->underlay_address || !s->rule_count)
		return -EINVAL;
	for (i = 1; i <= VPT1_MAX_PEERS; i++) {
		const struct vpt1_cfg_peer *p = &s->peers[i];

		if (!p->present) {
			if (p->owned.count)
				return -EINVAL;
			continue;
		}
		present++;
		if (p->slot != i || p->role > 1 || !p->endpoint || !p->port ||
		    p->pmtu < 1280 || p->pmtu > 1500 || !p->owned.count ||
		    p->owned.count > VPT1_ACL_MAX_PREFIXES)
			return -EINVAL;
		/* The local identity is not a peer's, and pins are distinct. */
		if (!memcmp(p->key, s->local_key, VPT1_KEY_BYTES))
			return -EINVAL;
		for (j = 1; j < i; j++) {
			const struct vpt1_cfg_peer *q = &s->peers[j];

			if (!q->present)
				continue;
			if (!memcmp(p->key, q->key, VPT1_KEY_BYTES))
				return -EEXIST;
			if (p->endpoint == q->endpoint && p->port == q->port)
				return -EEXIST;
		}
	}
	return present ? 0 : -EINVAL;
}

int vpt1_gw_validate(struct vpt1_gw *gw, u64 revision,
		     const struct vpt1_snapshot *s)
{
	struct vpt1_cfg *cfg, *cur;
	struct vpt1_acl_domain domain;
	size_t bound;
	unsigned int i;
	int err;

	lockdep_assert_held(&gw->cfg_lock);
	cur = rcu_dereference_protected(gw->cfg, lockdep_is_held(&gw->cfg_lock));
	if (gw->stopping)
		return -ENODEV;
	if (cur && revision <= cur->revision)
		return -ESTALE;
	if (!revision)
		return -EINVAL;
	err = snapshot_check(s);
	if (err)
		return err;
	domain = s->domain;
	for (i = 1; i <= VPT1_MAX_PEERS; i++)
		domain.peers[i - 1] = s->peers[i].owned;

	bound = vpt1_acl_charge_bound(s->rule_count);
	if (!bound)
		return -EINVAL;
	bound += kmalloc_size_roundup(sizeof(*cfg));
	/* Reserve in the shared budget before allocating; release on failure. */
	if (atomic_long_add_return(bound, &gw->acct->charged) > VPT1_CFG_BUDGET) {
		atomic_long_sub(bound, &gw->acct->charged);
		return -ENOSPC;
	}
	/* The same bytes also count against the 16 MiB/128 MiB aggregate ceilings. */
	if (!vpt1_gw_mem_reserve(gw, bound)) {
		atomic_long_sub(bound, &gw->acct->charged);
		return -ENOSPC;
	}
	cfg = gw_zalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg) {
		vpt1_gw_mem_release(gw, bound);
		atomic_long_sub(bound, &gw->acct->charged);
		return -ENOMEM;
	}
	cfg->acl = vpt1_acl_create(&domain, s->rules, s->rule_count);
	if (IS_ERR(cfg->acl)) {
		err = PTR_ERR(cfg->acl);
		vpt1_gw_mem_release(gw, bound);
		atomic_long_sub(bound, &gw->acct->charged);
		kfree(cfg);
		return err;
	}
	refcount_inc(&gw->acct->refs);
	cfg->acct = gw->acct;
	cfg->charge = bound;
	cfg->revision = revision;
	spin_lock_bh(&gw->lease_lock);
	cfg->owner_epoch = gw->live_epoch;
	spin_unlock_bh(&gw->lease_lock);
	cfg->lan_ifindex = s->lan_ifindex;
	cfg->underlay_ifindex = s->underlay_ifindex;
	cfg->underlay_address = s->underlay_address;
	memcpy(cfg->local_key, s->local_key, VPT1_KEY_BYTES);
	cfg->lan = s->domain.lan;
	cfg->underlay = s->domain.underlay;
	cfg->lan_address = s->domain.lan_address;
	for (i = 1; i <= VPT1_MAX_PEERS; i++)
		cfg->peers[i] = s->peers[i];
	atomic_long_inc(&vpt1_cfg_count);

	/* A newer VALIDATE replaces an older staged revision. */
	if (gw->staged)
		cfg_discard(gw->staged);
	gw->staged = cfg;
	gw->staged_epoch = cfg->owner_epoch;
	return 0;
}

/*
 * Sparse tracks one lock count per function and cannot follow a peer lock that is taken in one
 * function and released in another (admission keeps it from vpt1_admit_begin() to handoff, abort
 * or failure, tracked by ctx->locked), nor a count that depends on a loop (lock_peers()). The lock
 * calls below are hidden from it; the code itself is unchanged.
 */
#ifdef __CHECKER__
#define peer_lock_nested(peer, subclass) ((void)(peer), (void)(subclass))
#define peer_unlock_plain(peer) ((void)(peer))
#define peer_lock_bh(peer) ((void)(peer))
#define peer_unlock_bh(peer) ((void)(peer))
#else
#define peer_lock_nested(peer, subclass) spin_lock_nested(&(peer)->lock, subclass)
#define peer_unlock_plain(peer) spin_unlock(&(peer)->lock)
#define peer_lock_bh(peer) spin_lock_bh(&(peer)->lock)
#define peer_unlock_bh(peer) spin_unlock_bh(&(peer)->lock)
#endif

static void lock_peers(struct vpt1_peer *peers[VPT1_MAX_PEERS + 1])
{
	unsigned int slot;

	local_bh_disable();
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++)
		if (peers[slot])
			peer_lock_nested(peers[slot], slot);
}

static void unlock_peers(struct vpt1_peer *peers[VPT1_MAX_PEERS + 1])
{
	unsigned int slot;

	for (slot = VPT1_MAX_PEERS; slot >= 1; slot--)
		if (peers[slot])
			peer_unlock_plain(peers[slot]);
	local_bh_enable();
}

int vpt1_gw_commit(struct vpt1_gw *gw, u64 revision, u64 now_ns, bool *disruptive)
{
	struct vpt1_peer *old[VPT1_MAX_PEERS + 1] = {};
	struct vpt1_peer *new[VPT1_MAX_PEERS + 1] = {};
	struct vpt1_gens dropped[VPT1_MAX_PEERS + 1] = {};
	struct vpt1_cfg *cur, *staged;
	bool global_changed = false, disrupt = false, rebound = false;
	unsigned int slot;
	u64 live, deadline;
	u16 mtu = VPT1_INNER_CAP_MAX;
	int err;

	lockdep_assert_held(&gw->cfg_lock);
	*disruptive = false;
	if (gw->stopping)
		return -ENODEV;
	staged = gw->staged;
	cur = rcu_dereference_protected(gw->cfg, lockdep_is_held(&gw->cfg_lock));
	if (!staged || staged->revision != revision)
		return -ENOENT;
	if (cur && revision <= cur->revision)
		return -ESTALE;
	spin_lock_bh(&gw->lease_lock);
	live = gw->live_epoch;
	deadline = gw->deadline_ns;
	spin_unlock_bh(&gw->lease_lock);
	/* A snapshot staged by an old owner never commits (section 3). */
	if (!live || now_ns >= deadline || gw->staged_epoch != live)
		return -ESTALE;
	/* The ceiling holds until reclamation has actually run (section 4). */
	if (cur && atomic_read(&gw->acct->retired) >= VPT1_MAX_RETIRED)
		return -EBUSY;

	if (!cur || !global_equal(cur, staged))
		global_changed = true;
	/* Allocate every replacement before taking any peer lock. */
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		old[slot] = rcu_dereference_protected(
			gw->peers[slot], lockdep_is_held(&gw->cfg_lock));
		if (!staged->peers[slot].present)
			continue;
		mtu = min(mtu, vpt1_ip_inner_cap(staged->peers[slot].pmtu));
		if (old[slot] && cur && !global_changed &&
		    READ_ONCE(old[slot]->state) != VPT1_PEER_INVALID &&
		    peer_cfg_equal(&cur->peers[slot], &staged->peers[slot]))
			continue; /* Policy-only for this slot: keep the session. */
		new[slot] = peer_alloc(gw, slot, live);
		if (!new[slot])
			goto nomem;
		new[slot]->role = staged->peers[slot].role;
	}

	/*
	 * Gate old sessions before changing the socket's global binding. Failure
	 * preserves cfg/staging but leaves these sessions closed; recovery requires
	 * fresh peers, never reopening the retired keys. A new owner's first cfg
	 * also rebinds any socket retained after old-owner cleanup.
	 */
	if (vpt1_gw_ops_ptr && vpt1_gw_ops_ptr->bind && (!gw->bound || global_changed)) {
		if (global_changed) {
			for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
				if (old[slot]) {
					peer_invalidate(old[slot]);
					peer_drain(old[slot]);
				}
			}
		}
		if (gw->bound && vpt1_gw_ops_ptr->unbind) {
			vpt1_gw_ops_ptr->unbind(gw);
			gw->bound = false;
		}
		err = vpt1_gw_ops_ptr->bind(gw, staged->underlay_address,
					    staged->underlay_ifindex);
		if (err) {
			for (slot = 1; slot <= VPT1_MAX_PEERS; slot++)
				if (new[slot])
					vpt1_peer_put(new[slot]);
			return err;
		}
		gw->bound = true;
		rebound = true;
	}

	/* No fallible work follows this device transaction. Lower before a new
	 * smaller-budget peer can activate; RX still uses that peer's own cap.
	 */
	if (vpt1_gw_ops_ptr && vpt1_gw_ops_ptr->set_mtu) {
		err = vpt1_gw_ops_ptr->set_mtu(gw, mtu);
		if (err) {
			/* Do not retain an uncommitted candidate listener. Existing
			 * same-binding policy/MTU updates keep their original socket.
			 */
			if (rebound && vpt1_gw_ops_ptr->unbind) {
				vpt1_gw_ops_ptr->unbind(gw);
				gw->bound = false;
			}
			goto free_new;
		}
	}

	lock_peers(old);
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *p = old[slot];

		if (!p)
			continue;
		if (new[slot] || !staged->peers[slot].present) {
			p->state = VPT1_PEER_INVALID;
			peer_take_gens_locked(p, &dropped[slot]);
			disrupt = true;
		}
	}
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		if (new[slot] || !staged->peers[slot].present)
			rcu_assign_pointer(gw->peers[slot], new[slot]);
	}
	rcu_assign_pointer(gw->cfg, staged);
	gw->staged = NULL;
	unlock_peers(old);

	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		gens_release(&dropped[slot]);
		if (old[slot] && (new[slot] || !staged->peers[slot].present)) {
			peer_drain(old[slot]);
			vpt1_peer_put(old[slot]); /* The array's reference. */
		}
	}
	if (cur)
		cfg_retire(cur);
	ctr_inc(gw, VPT1_E_COMMITS);
	if (disrupt) {
		ctr_inc(gw, VPT1_E_DISRUPTIVE_COMMITS);
		*disruptive = true;
	}
	return 0;
nomem:
	err = -ENOMEM;
free_new:
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++)
		if (new[slot])
			vpt1_peer_put(new[slot]);
	return err;
}

static struct vpt1_peer *peer_locked(struct vpt1_gw *gw, unsigned int slot)
{
	return rcu_dereference_protected(gw->peers[slot],
					 lockdep_is_held(&gw->cfg_lock));
}

static bool locator_in_use(const struct vpt1_peer *p, u64 locator)
{
	return READ_ONCE(p->local_rx) == locator || READ_ONCE(p->next_local_rx) == locator ||
	       READ_ONCE(p->prev_local_rx) == locator;
}

/* cfg_lock held. */
static bool attempt_rx_in_use(struct vpt1_gw *gw, u64 rx)
{
	unsigned int i;

	for (i = 1; i <= VPT1_MAX_PEERS; i++) {
		struct vpt1_peer *p = peer_locked(gw, i);

		if (p && (locator_in_use(p, rx) || (p->att_valid && p->att_rx == rx)))
			return true;
	}
	return false;
}

void vpt1_peer_note_init(struct vpt1_peer *peer, const u8 id[VPT1_ATTEMPT_ID_LEN], u64 now_ns)
{
	spin_lock_bh(&peer->lock);
	memcpy(peer->init_id, id, VPT1_ATTEMPT_ID_LEN);
	peer->init_valid = true;
	peer->init_deadline_ns = now_ns + VPT1_ATTEMPT_NS;
	spin_unlock_bh(&peer->lock);
}

int vpt1_gw_attempt_begin(struct vpt1_gw *gw, unsigned int slot, const struct vpt1_incarnation *inc,
			  const u8 *given, u64 now_ns, u8 id[VPT1_ATTEMPT_ID_LEN], u64 *local_rx)
{
	static const u8 zero[VPT1_ATTEMPT_ID_LEN];
	struct vpt1_peer *peer;
	u64 rx;
	int err = 0;

	lockdep_assert_held(&gw->cfg_lock);
	if (gw->stopping)
		return -ENODEV;
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	if (given) {
		spin_lock_bh(&peer->lock);
		if (!peer->init_valid || now_ns >= peer->init_deadline_ns ||
		    memcmp(peer->init_id, given, VPT1_ATTEMPT_ID_LEN))
			err = -ENOENT;
		spin_unlock_bh(&peer->lock);
		if (err)
			return err;
		memcpy(id, given, VPT1_ATTEMPT_ID_LEN);
	} else {
		do {
			get_random_bytes(id, VPT1_ATTEMPT_ID_LEN);
		} while (!memcmp(id, zero, VPT1_ATTEMPT_ID_LEN));
	}
	do {
		get_random_bytes(&rx, sizeof(rx));
	} while (!rx || attempt_rx_in_use(gw, rx));
	spin_lock_bh(&peer->lock);
	if (memcmp(peer->inc.id, inc->id, VPT1_ID_LEN) || peer->state == VPT1_PEER_INVALID) {
		err = -ESTALE;
	} else {
		peer->att_valid = true; /* Replaces any earlier pending attempt. */
		peer->att_done = false;
		memcpy(peer->att_id, id, VPT1_ATTEMPT_ID_LEN);
		peer->att_rx = rx;
		peer->att_deadline_ns = now_ns + VPT1_ATTEMPT_NS;
		*local_rx = rx;
	}
	spin_unlock_bh(&peer->lock);
	return err;
}

int vpt1_gw_attempt_check(struct vpt1_gw *gw, unsigned int slot, const struct vpt1_incarnation *inc,
			  const u8 id[VPT1_ATTEMPT_ID_LEN], u64 local_rx, u64 now_ns, bool staging)
{
	struct vpt1_peer *peer;
	int err = 0;

	lockdep_assert_held(&gw->cfg_lock);
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	spin_lock_bh(&peer->lock);
	if (inc && memcmp(peer->inc.id, inc->id, VPT1_ID_LEN))
		err = -ESTALE;
	else if (!peer->att_valid || memcmp(peer->att_id, id, VPT1_ATTEMPT_ID_LEN))
		err = -ENOENT;
	else if (now_ns >= peer->att_deadline_ns)
		err = -ETIMEDOUT;
	else if (staging && peer->att_done)
		err = -ENOENT;
	else if (local_rx && local_rx != peer->att_rx)
		err = -EINVAL;
	spin_unlock_bh(&peer->lock);
	return err;
}

void vpt1_gw_attempt_done(struct vpt1_gw *gw, unsigned int slot, const u8 id[VPT1_ATTEMPT_ID_LEN],
			  u64 now_ns)
{
	struct vpt1_peer *peer;

	lockdep_assert_held(&gw->cfg_lock);
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return;
	spin_lock_bh(&peer->lock);
	if (peer->att_valid && !memcmp(peer->att_id, id, VPT1_ATTEMPT_ID_LEN)) {
		peer->att_done = true;
		peer->att_deadline_ns = now_ns + VPT1_ATTEMPT_DONE_NS;
	}
	spin_unlock_bh(&peer->lock);
}

/*
 * Install a generation as the peer's candidate: it receives control records at once and carries
 * no application data until a local commit (CONFIRM / READY / COMMIT / COMMITTED, or
 * vpt1_peer_commit()). A new candidate replaces an uncommitted earlier one; the current session
 * is untouched until the commit. @mode 0 is a fresh establishment, whose commit starts a new
 * continuity and drops the old keys. @mode 1 keeps the session and needs a live continuity grant
 * for @attempt bound to the current generation's hash, which it consumes (-EPERM otherwise).
 */
int vpt1_gw_stage_generation(struct vpt1_gw *gw, unsigned int slot,
			     const struct vpt1_incarnation *inc,
			     struct vpt1_generation *gen, u64 local_rx, u64 remote_rx,
			     const u8 hash[VPT1_HASH_LEN], u8 mode,
			     const u8 attempt[VPT1_ATTEMPT_ID_LEN], const u8 *parent)
{
	struct vpt1_gens gens = {};
	struct vpt1_continuity *cont;
	struct vpt1_peer *peer;
	u64 now = ktime_get_boottime_ns();
	bool staged = false;
	unsigned int i;
	int err = 0;

	lockdep_assert_held(&gw->cfg_lock);
	if (gw->stopping)
		return -ENODEV;
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	/* Receive locators are nonzero hints, unique within the gateway. */
	if (!local_rx || !remote_rx)
		return -EINVAL;
	for (i = 1; i <= VPT1_MAX_PEERS; i++) {
		struct vpt1_peer *other = peer_locked(gw, i);

		if (other && locator_in_use(other, local_rx))
			return -EEXIST;
	}
	/* Allocated before the lock; used only when this candidate starts a new session. */
	cont = mode ? NULL : cont_alloc();
	if (!mode && !cont)
		return -ENOMEM;
	spin_lock_bh(&peer->lock);
	if (memcmp(peer->inc.id, inc->id, VPT1_ID_LEN) ||
	    peer->state == VPT1_PEER_INVALID) {
		err = -ESTALE;
	} else if (mode) {
		bool usable = peer->gen && vpt1_generation_limit(peer->gen, now) != VPT1_LIMIT_HARD &&
			      !live_closed_locked(peer, now);
		bool granted = false;

		if (usable && parent && !memcmp(parent, peer->gen_hash, VPT1_HASH_LEN)) {
			if (peer->role)
				granted = peer->rk_state == RK_GRANTED && now < peer->rk_deadline_ns &&
					  !memcmp(peer->rk_att, attempt, VPT1_ATTEMPT_ID_LEN) &&
					  !memcmp(peer->rk_parent, parent, VPT1_HASH_LEN);
			else
				granted = peer->grant_valid && !peer->grant_used &&
					  now < peer->grant_deadline_ns &&
					  !memcmp(peer->grant_att, attempt, VPT1_ATTEMPT_ID_LEN) &&
					  !memcmp(peer->grant_parent, parent, VPT1_HASH_LEN);
		}
		if (!granted)
			err = -EPERM;
	}
	if (!err) {
		gens.g[1] = peer->next_gen; /* An uncommitted candidate this one replaces. */
		gens.cont2 = peer->next_cont;
		peer->next_gen = gen;
		WRITE_ONCE(peer->next_local_rx, local_rx);
		peer->next_remote_rx = remote_rx;
		peer->next_deadline_ns = now + VPT1_ATTEMPT_NS;
		peer->act_deadline_ns = peer->next_deadline_ns;
		memcpy(peer->next_hash, hash, VPT1_HASH_LEN);
		memcpy(peer->next_att, attempt, VPT1_ATTEMPT_ID_LEN);
		peer->next_fresh = !mode;
		peer->next_cont = cont; /* A fresh candidate brings its own continuity. */
		cont = NULL;
		if (mode) {
			if (peer->role)
				peer->rk_state = RK_NONE;
			else
				peer->grant_used = true; /* One candidate per grant. */
		}
		peer->act_state = peer->role ? ACT_WAIT_READY : ACT_NONE;
		peer->act_step = 0;
		peer->act_due_ns = now; /* The initiator's CONFIRM goes out at once. */
		peer->state = VPT1_PEER_READY;
		staged = true;
	}
	spin_unlock_bh(&peer->lock);
	gens_release(&gens);
	vpt1_continuity_put(cont); /* Unused on a mode 1 or failed stage. */
	if (staged)
		act_schedule(gw, 0);
	return err;
}

int vpt1_gw_stage_duplicate(struct vpt1_gw *gw, unsigned int slot,
			    const struct vpt1_incarnation *inc,
			    const u8 attempt[VPT1_ATTEMPT_ID_LEN], u64 local_rx, u64 remote_rx,
			    const u8 hash[VPT1_HASH_LEN])
{
	struct vpt1_peer *peer;
	int r = 0;

	lockdep_assert_held(&gw->cfg_lock);
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	spin_lock_bh(&peer->lock);
	if (memcmp(peer->inc.id, inc->id, VPT1_ID_LEN)) {
		r = -ESTALE;
	} else if (peer->next_gen && !memcmp(peer->next_att, attempt, VPT1_ATTEMPT_ID_LEN)) {
		r = peer->next_local_rx == local_rx && peer->next_remote_rx == remote_rx &&
		    !memcmp(peer->next_hash, hash, VPT1_HASH_LEN) ? 1 : -EEXIST;
	} else if (peer->gen && !memcmp(peer->gen_att, attempt, VPT1_ATTEMPT_ID_LEN)) {
		r = peer->local_rx == local_rx && peer->remote_rx == remote_rx &&
		    !memcmp(peer->gen_hash, hash, VPT1_HASH_LEN) ? 1 : -EEXIST;
	}
	spin_unlock_bh(&peer->lock);
	return r;
}

int vpt1_gw_rekey_request(struct vpt1_gw *gw, unsigned int slot,
			  const struct vpt1_incarnation *inc,
			  const u8 attempt[VPT1_ATTEMPT_ID_LEN], u64 now_ns)
{
	struct vpt1_peer *peer;
	int err = 0;

	lockdep_assert_held(&gw->cfg_lock);
	if (gw->stopping)
		return -ENODEV;
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	spin_lock_bh(&peer->lock);
	if (memcmp(peer->inc.id, inc->id, VPT1_ID_LEN) || peer->state == VPT1_PEER_INVALID)
		err = -ESTALE;
	else if (!peer->role)
		err = -EPERM; /* The designated initiator coordinates a re-key. */
	else if (!peer->gen || live_closed_locked(peer, now_ns) ||
		 vpt1_generation_limit(peer->gen, now_ns) == VPT1_LIMIT_HARD)
		err = -ENOTCONN;
	else if (!peer->att_valid || peer->att_done || now_ns >= peer->att_deadline_ns ||
		 memcmp(peer->att_id, attempt, VPT1_ATTEMPT_ID_LEN))
		err = -ENOENT;
	if (!err) {
		peer->rk_state = RK_REQUESTED;
		memcpy(peer->rk_att, attempt, VPT1_ATTEMPT_ID_LEN);
		memcpy(peer->rk_parent, peer->gen_hash, VPT1_HASH_LEN);
		peer->rk_step = 0;
		peer->rk_due_ns = now_ns;
		peer->rk_deadline_ns = now_ns + VPT1_ATTEMPT_NS;
	}
	spin_unlock_bh(&peer->lock);
	if (!err)
		act_schedule(gw, 0);
	return err;
}

#ifdef VPT1_DEBUG
/*
 * Unit tests that predate activation: install like the old STAGE_GENERATION did. A usable session
 * takes the generation as a continuity-keeping candidate (mode 1, the grant forged here as the
 * protocol would have made it) and leaves the commit to the test; otherwise it is a fresh session
 * and is committed at once. cfg_lock held.
 */
int vpt1_gw_dbg_stage(struct vpt1_gw *gw, unsigned int slot, const struct vpt1_incarnation *inc,
		      struct vpt1_generation *gen, u64 local_rx, u64 remote_rx)
{
	u8 hash[VPT1_HASH_LEN], att[VPT1_ATTEMPT_ID_LEN], parent[VPT1_HASH_LEN];
	u64 now = ktime_get_boottime_ns();
	struct vpt1_peer *peer;
	int err;

	lockdep_assert_held(&gw->cfg_lock);
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	memset(hash, 0x5a, sizeof(hash));
	memset(att, 0xa5, sizeof(att));
	memcpy(att, &local_rx, sizeof(local_rx));
	spin_lock_bh(&peer->lock);
	memcpy(parent, peer->gen_hash, sizeof(parent));
	if (peer->role) {
		peer->rk_state = RK_GRANTED;
		memcpy(peer->rk_att, att, sizeof(att));
		memcpy(peer->rk_parent, parent, sizeof(parent));
		peer->rk_deadline_ns = now + VPT1_ATTEMPT_NS;
	} else {
		peer->grant_valid = true;
		peer->grant_used = false;
		memcpy(peer->grant_att, att, sizeof(att));
		memcpy(peer->grant_parent, parent, sizeof(parent));
		peer->grant_deadline_ns = now + VPT1_ATTEMPT_NS;
	}
	spin_unlock_bh(&peer->lock);
	err = vpt1_gw_stage_generation(gw, slot, inc, gen, local_rx, remote_rx, hash, 1, att, parent);
	if (err == -EPERM) {
		err = vpt1_gw_stage_generation(gw, slot, inc, gen, local_rx, remote_rx, hash, 0, att,
					       NULL);
		if (!err)
			vpt1_peer_commit(peer, now);
	}
	return err;
}
#endif

int vpt1_gw_disable_peer(struct vpt1_gw *gw, unsigned int slot,
			 const struct vpt1_incarnation *inc)
{
	struct vpt1_gens gens = {};
	struct vpt1_peer *peer;
	int err = 0;

	lockdep_assert_held(&gw->cfg_lock);
	peer = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!peer)
		return -ENOENT;
	spin_lock_bh(&peer->lock);
	if (memcmp(peer->inc.id, inc->id, VPT1_ID_LEN) ||
	    peer->state == VPT1_PEER_INVALID) {
		err = -ESTALE;
	} else {
		peer->state = VPT1_PEER_DISABLED;
		peer_take_gens_locked(peer, &gens);
	}
	spin_unlock_bh(&peer->lock);
	if (!err) {
		peer_frag_flush(peer, gens.cont);
		peer_err_clear(peer);
	}
	gens_release(&gens);
	return err;
}

/* Revoke replaces the peer: a new incarnation, so a fresh session is required. */
int vpt1_gw_revoke_peer(struct vpt1_gw *gw, unsigned int slot,
			const struct vpt1_incarnation *inc)
{
	struct vpt1_peer *old, *new;
	u64 epoch;

	lockdep_assert_held(&gw->cfg_lock);
	old = slot >= 1 && slot <= VPT1_MAX_PEERS ? peer_locked(gw, slot) : NULL;
	if (!old)
		return -ENOENT;
	if (memcmp(old->inc.id, inc->id, VPT1_ID_LEN))
		return -ESTALE;
	epoch = old->owner_epoch;
	new = peer_alloc(gw, slot, epoch);
	if (!new)
		return -ENOMEM;
	new->role = old->role;
	/* Admission is entirely before or after: the swap is made under the lock. */
	spin_lock_bh(&old->lock);
	old->state = VPT1_PEER_INVALID;
	rcu_assign_pointer(gw->peers[slot], new);
	spin_unlock_bh(&old->lock);
	{
		struct vpt1_gens gens;

		spin_lock_bh(&old->lock);
		peer_take_gens_locked(old, &gens);
		spin_unlock_bh(&old->lock);
		gens_release(&gens);
	}
	peer_drain(old);
	vpt1_peer_put(old);
	return 0;
}

int vpt1_gw_status(struct vpt1_gw *gw, u64 now_ns, struct vpt1_status *out)
{
	struct vpt1_cfg *cfg;
	unsigned int slot;

	lockdep_assert_held(&gw->cfg_lock);
	memset(out, 0, sizeof(*out));
	cfg = rcu_dereference_protected(gw->cfg, lockdep_is_held(&gw->cfg_lock));
	spin_lock_bh(&gw->lease_lock);
	out->stopping = gw->stopping;
	if (gw->live_epoch && now_ns < gw->deadline_ns) {
		out->owned = true;
		out->owner_epoch = gw->live_epoch;
		out->lease_remaining_ms = div_u64(gw->deadline_ns - now_ns, 1000000ULL);
	}
	spin_unlock_bh(&gw->lease_lock);
	out->revision = cfg ? cfg->revision : 0;
	if (gw->staged) {
		out->has_staged = true;
		out->staged_revision = gw->staged->revision;
	}
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *peer = peer_locked(gw, slot);
		struct vpt1_status_peer *sp;

		if (!peer || !cfg)
			continue;
		sp = &out->peers[out->peer_count++];
		sp->slot = slot;
		sp->pmtu = cfg->peers[slot].pmtu;
		sp->endpoint = cfg->peers[slot].endpoint;
		sp->port = cfg->peers[slot].port;
		sp->incarnation = peer->inc;
		spin_lock_bh(&peer->lock);
		sp->state = peer->state;
		sp->has_generation = peer->gen != NULL;
		spin_unlock_bh(&peer->lock);
	}
	return 0;
}

void vpt1_gw_counters(const struct vpt1_gw *gw, u64 out[VPT1_E_COUNT])
{
	unsigned int i;

	for (i = 0; i < VPT1_E_COUNT; i++)
		out[i] = atomic64_read(&gw->ctr[i]);
}

void vpt1_gw_count_rejected(struct vpt1_gw *gw)
{
	ctr_inc(gw, VPT1_E_REJECTED);
}

int vpt1_gw_owner_portid(struct vpt1_gw *gw, u32 *portid)
{
	int err = 0;

	spin_lock_bh(&gw->lease_lock);
	if (gw->stopping || !gw->live_epoch)
		err = -ENOTCONN;
	else
		*portid = gw->portid;
	spin_unlock_bh(&gw->lease_lock);
	return err;
}

/* Teardown (contract section 7) ---------------------------------------------- */

void vpt1_gw_close(struct vpt1_gw *gw)
{
	struct vpt1_peer *peers[VPT1_MAX_PEERS + 1] = {};
	unsigned int slot;

	spin_lock_bh(&gw->lease_lock);
	WRITE_ONCE(gw->stopping, true);
	WRITE_ONCE(gw->live_epoch, 0);
	spin_unlock_bh(&gw->lease_lock);
	rcu_read_lock();
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++)
		peers[slot] = rcu_dereference(gw->peers[slot]);
	/* Every peer becomes ineligible under its own lock, ascending slot order. */
	lock_peers(peers);
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++)
		if (peers[slot]) {
			peers[slot]->state = VPT1_PEER_INVALID;
			if (peers[slot]->cont)
				atomic_set(&peers[slot]->cont->dead, 1);
		}
	unlock_peers(peers);
	rcu_read_unlock();
}

void vpt1_gw_close_all(void)
{
	struct vpt1_gw *gw;

	spin_lock(&vpt1_gw_list_lock);
	list_for_each_entry(gw, &vpt1_gw_list, node)
		vpt1_gw_close(gw);
	spin_unlock(&vpt1_gw_list_lock);
}

void vpt1_gw_destroy(struct vpt1_gw *gw, unsigned int held)
{
	unsigned int slot;
	struct vpt1_cfg *cfg;

	/* A: close admission before any wait. */
	vpt1_gw_close(gw);
	spin_lock(&vpt1_gw_list_lock);
	list_del_init(&gw->node);
	spin_unlock(&vpt1_gw_list_lock);
	cancel_delayed_work_sync(&gw->expiry_work);
	cancel_delayed_work_sync(&gw->gens_work);
	cancel_delayed_work_sync(&gw->live_work);
	cancel_delayed_work_sync(&gw->frag_work);
	cancel_delayed_work_sync(&gw->act_work);
	jobs_close(gw);
	/* B: detach the outer socket; no new datagram can reach this gateway. */
	if (gw->bound && vpt1_gw_ops_ptr && vpt1_gw_ops_ptr->unbind) {
		vpt1_gw_ops_ptr->unbind(gw);
		gw->bound = false;
	}
	/* The socket is gone, so no intake can arm the expiry work again. */
	cancel_delayed_work_sync(&gw->frag_work);
	/* C: wait for handlers and workers, outside every lock. */
	wait_var_event(&gw->users, refcount_read(&gw->users) == 1 + held);
	mutex_lock(&gw->cfg_lock);
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *peer = peer_locked(gw, slot);

		if (!peer)
			continue;
		peer_invalidate(peer);
		RCU_INIT_POINTER(gw->peers[slot], NULL);
		peer_drain(peer);
		vpt1_peer_put(peer);
	}
	/* D: unpublish; every config is freed by its own RCU callback. */
	cfg = rcu_dereference_protected(gw->cfg, lockdep_is_held(&gw->cfg_lock));
	RCU_INIT_POINTER(gw->cfg, NULL);
	cfg_retire(cfg);
	if (gw->staged) {
		cfg_discard(gw->staged);
		gw->staged = NULL;
	}
	mutex_unlock(&gw->cfg_lock);
	/* The creator's references; the gateway memory goes with the last peer. */
	/* Reaches zero only when no handler still holds a reference. */
	if (!refcount_dec_and_test(&gw->users))
		WARN_ON_ONCE(!held);
	gw_life_put(gw);
}

/* Data-path lookups ---------------------------------------------------------- */

static bool prefix_has(const struct vpt1_acl_prefix *p, __be32 addr)
{
	u32 mask = p->length ? ~0U << (32 - p->length) : 0;

	return !((ntohl(addr) ^ ntohl(p->address)) & mask);
}

bool vpt1_gw_underlay_local(struct vpt1_gw *gw, u32 ifindex, __be32 destination)
{
	const struct vpt1_cfg *cfg = rcu_dereference(gw->cfg);

	return cfg && cfg->underlay_ifindex == ifindex && cfg->underlay_address == destination;
}

int vpt1_gw_tx_route(struct vpt1_gw *gw, const struct vpt1_pkt_info *pi,
		     struct vpt1_route *route)
{
	const struct vpt1_cfg *cfg = rcu_dereference(gw->cfg);
	unsigned int slot, i;

	if (!cfg)
		return -ENOENT;
	/* Destination ownership picks the peer; the ACL later decides permission. */
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		const struct vpt1_cfg_peer *p = &cfg->peers[slot];

		if (!p->present)
			continue;
		for (i = 0; i < p->owned.count; i++) {
			if (!prefix_has(&p->owned.prefixes[i], pi->dst))
				continue;
			route->endpoint = p->endpoint;
			route->port = p->port;
			route->local = cfg->underlay_address;
			route->ifindex = cfg->underlay_ifindex;
			route->slot = slot;
			return 0;
		}
	}
	return -ENOENT;
}

struct vpt1_peer *vpt1_gw_peer_by_locator(struct vpt1_gw *gw, u64 locator)
{
	unsigned int slot;
	struct vpt1_peer *peer = NULL;

	if (!locator)
		return NULL;
	rcu_read_lock();
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *p = rcu_dereference(gw->peers[slot]);

		if (p && locator_in_use(p, locator) && refcount_inc_not_zero(&p->refs)) {
			peer = p;
			break;
		}
	}
	rcu_read_unlock();
	return peer;
}

bool vpt1_peer_source_ok(struct vpt1_peer *peer, __be32 saddr, __be16 sport)
{
	const struct vpt1_cfg *cfg = rcu_dereference(peer->gw->cfg);

	return cfg && cfg->peers[peer->slot].present &&
	       cfg->peers[peer->slot].endpoint == saddr &&
	       cfg->peers[peer->slot].port == sport;
}

int vpt1_gw_peer_route(struct vpt1_gw *gw, unsigned int slot, struct vpt1_route *route)
{
	const struct vpt1_cfg *cfg = rcu_dereference(gw->cfg);

	if (!cfg || slot < 1 || slot > VPT1_MAX_PEERS || !cfg->peers[slot].present)
		return -ENOENT;
	route->endpoint = cfg->peers[slot].endpoint;
	route->port = cfg->peers[slot].port;
	route->local = cfg->underlay_address;
	route->ifindex = cfg->underlay_ifindex;
	route->slot = slot;
	return 0;
}

struct vpt1_peer *vpt1_gw_peer_by_endpoint(struct vpt1_gw *gw, __be32 addr, __be16 port)
{
	const struct vpt1_cfg *cfg = rcu_dereference(gw->cfg);
	unsigned int slot;

	if (!cfg)
		return NULL;
	for (slot = 1; slot <= VPT1_MAX_PEERS; slot++) {
		struct vpt1_peer *peer;

		if (!cfg->peers[slot].present || cfg->peers[slot].endpoint != addr ||
		    cfg->peers[slot].port != port)
			continue;
		peer = rcu_dereference(gw->peers[slot]);
		if (peer && refcount_inc_not_zero(&peer->refs))
			return peer;
		return NULL;
	}
	return NULL;
}

bool vpt1_peer_exchange_admit(struct vpt1_peer *peer, u64 now_ns)
{
	bool ok;

	spin_lock_bh(&peer->lock);
	ok = bucket_take(&peer->exchange, 1, now_ns);
	spin_unlock_bh(&peer->lock);
	return ok;
}

u8 vpt1_peer_slot(const struct vpt1_peer *peer)
{
	return peer->slot;
}

struct vpt1_gw *vpt1_peer_gw(const struct vpt1_peer *peer)
{
	return peer->gw;
}

/* Live IP adapter (HR3/HR4) ---------------------------------------------------------- */

/* A unicast address inside @p: not the network or broadcast address of a prefix below /31. */
static bool prefix_unicast(const struct vpt1_acl_prefix *p, __be32 addr)
{
	u32 all = p->length ? ~(~0U << (32 - p->length)) : ~0U;
	u32 host = ntohl(addr) & all;

	if (!prefix_has(p, addr) || ipv4_is_multicast(addr) || ipv4_is_loopback(addr) ||
	    ipv4_is_lbcast(addr) || ipv4_is_zeronet(addr))
		return false;
	return p->length >= 31 || (host && host != all);
}

bool vpt1_gw_lan_has(struct vpt1_gw *gw, __be32 addr, bool allow_gateway_address)
{
	const struct vpt1_cfg *cfg = rcu_dereference(gw->cfg);

	if (!cfg || !prefix_unicast(&cfg->lan, addr))
		return false;
	return allow_gateway_address || addr != cfg->lan_address;
}

bool vpt1_peer_owns(struct vpt1_peer *peer, __be32 addr)
{
	const struct vpt1_cfg *cfg = rcu_dereference(peer->gw->cfg);
	const struct vpt1_cfg_peer *p;
	unsigned int i;

	if (!cfg || ipv4_is_multicast(addr) || ipv4_is_loopback(addr) || ipv4_is_lbcast(addr) ||
	    ipv4_is_zeronet(addr))
		return false;
	p = &cfg->peers[peer->slot];
	if (!p->present)
		return false;
	for (i = 0; i < p->owned.count; i++)
		if (prefix_has(&p->owned.prefixes[i], addr))
			return true;
	return false;
}

unsigned int vpt1_peer_cap(struct vpt1_peer *peer)
{
	const struct vpt1_cfg *cfg = rcu_dereference(peer->gw->cfg);

	return cfg && cfg->peers[peer->slot].present ?
	       vpt1_ip_inner_cap(cfg->peers[peer->slot].pmtu) : 0;
}

bool vpt1_peer_eligible(struct vpt1_peer *peer, u64 now_ns)
{
	struct vpt1_gw *gw = peer->gw;
	bool ok;

	if (READ_ONCE(gw->stopping) || !vpt1_gw_lease_live(gw, now_ns))
		return false;
	spin_lock_bh(&peer->lock);
	ok = peer->state == VPT1_PEER_READY && peer->owner_epoch == READ_ONCE(gw->live_epoch) &&
	     !live_closed_locked(peer, now_ns);
	spin_unlock_bh(&peer->lock);
	return ok;
}

static const struct vpt1_frag_ops vpt1_frag_ops = {
	.get = vpt1_continuity_try_get,
	.put = vpt1_continuity_put,
	.reserve = vpt1_gw_frag_reserve,
	.release = vpt1_gw_frag_release,
};

struct vpt1_frag_cache *vpt1_peer_frag_cache(struct vpt1_peer *peer)
{
	/* Pairs with the release below, for the lockless readers (flush, expiry). */
	struct vpt1_frag_cache *cache = smp_load_acquire(&peer->frag);

	if (cache)
		return cache;
	if (READ_ONCE(peer->state) == VPT1_PEER_INVALID)
		return ERR_PTR(-ESTALE);
	cache = vpt1_frag_create(&vpt1_frag_ops, peer->gw);
	if (!IS_ERR(cache))
		smp_store_release(&peer->frag, cache); /* Publishes the initialized cache. */
	return cache;
}

void vpt1_gw_frag_work_arm(struct vpt1_gw *gw)
{
	if (vpt1_gw_wq && !READ_ONCE(gw->stopping))
		queue_delayed_work(vpt1_gw_wq, &gw->frag_work, HZ);
}

/*
 * Once a second while a cache may hold a set: expire sets past their five seconds. The work takes
 * cfg_lock (like live_work) to walk the peers; it is not a queued job, so peer_drain() never
 * waits for it.
 */
static void frag_work_fn(struct work_struct *work)
{
	struct vpt1_gw *gw = container_of(to_delayed_work(work), struct vpt1_gw, frag_work);
	u64 now = ktime_get_boottime_ns();
	bool again = false;
	unsigned int slot;

	if (!vpt1_gw_get(gw))
		return;
	mutex_lock(&gw->cfg_lock);
	for (slot = 1; slot <= VPT1_MAX_PEERS && !gw->stopping; slot++) {
		struct vpt1_peer *peer = peer_locked(gw, slot);
		struct vpt1_frag_usage usage;

		if (!peer || !peer->frag)
			continue;
		vpt1_frag_expire(peer->frag, now);
		vpt1_frag_usage(peer->frag, &usage);
		if (usage.sets || usage.results)
			again = true;
	}
	mutex_unlock(&gw->cfg_lock);
	if (again)
		vpt1_gw_frag_work_arm(gw);
	vpt1_gw_put(gw);
}

void vpt1_peer_err_save(struct vpt1_peer *peer, enum vpt1_dir saved_in, u32 rule_id, bool reply,
			const u8 *packet, size_t length, u64 now_ns)
{
	struct vpt1_err_table *t = peer->errs;
	struct vpt1_recent_packet *rp;
	struct vpt1_err_dir *d;
	unsigned int i;

	if (!t || !rule_id || length < 28 || (saved_in != VPT1_DIR_TX && saved_in != VPT1_DIR_RX))
		return;
	d = &t->dir[saved_in];
	spin_lock_bh(&t->lock);
	i = d->next;
	d->next = (i + 1) % VPT1_ERROR_CONTEXTS;
	rp = &d->slot[i];
	rp->created_ns = now_ns;
	rp->rule_id = rule_id;
	rp->kind = VPT1_RECENT_INNER;
	rp->reserved = 0;
	rp->prefix_len = min_t(size_t, length, VPT1_ERROR_PREFIX);
	memcpy(rp->prefix, packet, rp->prefix_len);
	__assign_bit(i, d->reply, reply);
	spin_unlock_bh(&t->lock);
}

/* The original initiating tuple behind a saved packet, as packet_rule() built it. */
static void err_flow(const struct vpt1_recent_packet *rp, bool reply, enum vpt1_dir saved_in,
		     u8 slot, struct vpt1_acl_flow *f)
{
	const u8 *p = rp->prefix;
	__be32 src, dst;
	__be16 sport = 0, dport = 0;

	memcpy(&src, p + 12, 4);
	memcpy(&dst, p + 16, 4);
	if (p[9] == IPPROTO_TCP || p[9] == IPPROTO_UDP) {
		memcpy(&sport, p + 20, 2);
		memcpy(&dport, p + 22, 2);
	}
	memset(f, 0, sizeof(*f));
	f->peer_slot = slot;
	f->protocol = p[9];
	if (p[9] == IPPROTO_ICMP)
		f->icmp_type = ICMP_ECHO;
	if (reply) {
		f->source = dst;
		f->destination = src;
		f->source_port = dport;
		f->destination_port = sport;
	} else {
		f->source = src;
		f->destination = dst;
		f->source_port = sport;
		f->destination_port = dport;
	}
	/* Travelled to the peer: an initiation was local to peer. Received: the other way. */
	f->direction = (saved_in == VPT1_DIR_TX) != reply ? VPT1_ACL_LOCAL_TO_PEER :
							    VPT1_ACL_PEER_TO_LOCAL;
}

int vpt1_peer_err_find(struct vpt1_peer *peer, enum vpt1_dir saved_in, const u8 *error,
		       size_t length, u64 now_ns, struct vpt1_error_ctx *out)
{
	struct vpt1_err_table *t = peer->errs;
	const struct vpt1_recent_packet *pick = NULL;
	bool pick_reply = false, ambiguous = false;
	struct vpt1_err_dir *d;
	unsigned int i;

	if (!t || !error || !out || length < 56 ||
	    (saved_in != VPT1_DIR_TX && saved_in != VPT1_DIR_RX))
		return -EINVAL;
	d = &t->dir[saved_in];
	spin_lock_bh(&t->lock);
	for (i = 0; i < VPT1_ERROR_CONTEXTS; i++) {
		const struct vpt1_recent_packet *rp = &d->slot[i];

		if (!rp->prefix_len || memcmp(rp->prefix + 12, error + 28 + 12, 8))
			continue; /* Cheap filter: the quoted source and destination. */
		if (vpt1_recent_match(rp, error, length, now_ns))
			continue;
		if (!pick) {
			pick = rp;
			pick_reply = test_bit(i, d->reply);
		} else if (pick->rule_id != rp->rule_id || pick_reply != test_bit(i, d->reply)) {
			ambiguous = true;
		}
	}
	if (pick && !ambiguous) {
		memset(out, 0, sizeof(*out));
		out->created_ns = pick->created_ns;
		out->rule_id = pick->rule_id;
		out->held = true;
		out->validated = true;
		out->peer_slot = peer->slot;
		err_flow(pick, pick_reply, saved_in, peer->slot, &out->flow);
	}
	spin_unlock_bh(&t->lock);
	return !pick ? -ENOENT : ambiguous ? -ENOTUNIQ : 0;
}

/* Final admission (contract section 6) --------------------------------------- */

static void admit_fail(struct vpt1_admit_ctx *ctx, enum vpt1_reason why,
		       enum vpt1_reason *reason)
{
	if (ctx->locked) {
		peer_unlock_bh(ctx->peer);
		ctx->locked = false;
	}
	ctx->authorized = false;
	if (reason)
		*reason = why;
	reason_inc(ctx->peer->gw, why);
}

static int admit_begin(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
		       enum vpt1_dir dir, struct vpt1_generation *gen, u64 now_ns,
		       struct vpt1_admit_ctx *ctx, bool reserved, enum vpt1_reason *reason)
{
	struct vpt1_gw *gw = peer->gw;
	u64 live;
	int err;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "vpt1_admit_begin() needs rcu_read_lock()");
	memset(ctx, 0, sizeof(*ctx));
	ctx->peer = peer;
	ctx->gen = gen;
	ctx->dir = dir;
	ctx->now_ns = now_ns;
	peer_lock_bh(peer);
	ctx->locked = true;

	if (READ_ONCE(gw->stopping)) {
		admit_fail(ctx, VPT1_R_STOPPING, reason);
		return -ESHUTDOWN;
	}
	/* The peer is admissible only under the live owner epoch and lease. */
	live = READ_ONCE(gw->live_epoch);
	if (!live || peer->owner_epoch != live) {
		admit_fail(ctx, VPT1_R_NO_OWNER, reason);
		return -EPERM;
	}
	if (now_ns >= READ_ONCE(gw->deadline_ns)) {
		admit_fail(ctx, VPT1_R_LEASE_EXPIRED, reason);
		return -ESTALE;
	}
	if (peer->state != VPT1_PEER_READY) {
		admit_fail(ctx, VPT1_R_PEER_INELIGIBLE, reason);
		return -EACCES;
	}
	if (memcmp(peer->inc.id, held->id, VPT1_ID_LEN)) {
		admit_fail(ctx, VPT1_R_STALE_INCARNATION, reason);
		return -ESTALE;
	}
	/* Sending uses the current generation only; receiving also accepts next and prev. */
	if (!gen || !peer->gen ||
	    !(gen == peer->gen ||
	      (dir == VPT1_DIR_RX &&
	       (gen == peer->next_gen ||
		(gen == peer->prev_gen && now_ns < peer->prev_deadline_ns))))) {
		admit_fail(ctx, VPT1_R_NO_GENERATION, reason);
		return -ENOKEY;
	}
	if (reserved)
		err = vpt1_generation_reserved_tx_valid(gen, now_ns);
	else
		err = vpt1_generation_limit(gen, now_ns) == VPT1_LIMIT_HARD ? -EKEYEXPIRED : 0;
	if (err) {
		admit_fail(ctx, VPT1_R_GENERATION_LIMIT, reason);
		return err;
	}
	/* No authenticated round trip for VPT1_LIVE_NS: closed until a new session. */
	if (live_closed_locked(peer, now_ns)) {
		admit_fail(ctx, VPT1_R_PEER_INELIGIBLE, reason);
		return -EACCES;
	}
	/* The current config, never a pointer the caller kept from earlier. */
	ctx->cfg = rcu_dereference(gw->cfg);
	if (!ctx->cfg) {
		admit_fail(ctx, VPT1_R_NO_CONFIG, reason);
		return -ENOENT;
	}
	ctx->revision = ctx->cfg->revision;
	if (reason)
		*reason = VPT1_R_OK;
	return 0;
}

int vpt1_admit_begin(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
		     enum vpt1_dir dir, struct vpt1_generation *gen, u64 now_ns,
		     struct vpt1_admit_ctx *ctx, enum vpt1_reason *reason)
{
	return admit_begin(peer, held, dir, gen, now_ns, ctx, false, reason);
}

static int admit_inflight(struct vpt1_admit_ctx *ctx, enum vpt1_reason *reason)
{
	if (atomic_read(&ctx->peer->inflight) >= VPT1_PEER_INFLIGHT_MAX) {
		admit_fail(ctx, VPT1_R_INFLIGHT_LIMIT, reason);
		return -EBUSY;
	}
	return 0;
}

int vpt1_admit_existing(struct vpt1_admit_ctx *ctx, const struct vpt1_witness *w,
			enum vpt1_reason *reason)
{
	if (!ctx->locked)
		return -EINVAL;
	if (!w || !w->valid || !w->rule_id) {
		admit_fail(ctx, VPT1_R_NO_WITNESS, reason);
		return -ENOENT;
	}
	if (w->flow.peer_slot != ctx->peer->slot) {
		admit_fail(ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	/* The saved rule ID and original tuple, against the CURRENT ACL. */
	if (!vpt1_acl_allows_rule(ctx->cfg->acl, w->rule_id, &w->flow)) {
		admit_fail(ctx, VPT1_R_ACL_DENY, reason);
		return -EACCES;
	}
	if (admit_inflight(ctx, reason))
		return -EBUSY;
	ctx->rule_id = w->rule_id;
	ctx->authorized = true;
	return 0;
}

int vpt1_admit_new(struct vpt1_admit_ctx *ctx, const struct vpt1_new_initiation *init,
		   enum vpt1_reason *reason)
{
	u32 rule;

	if (!ctx->locked)
		return -EINVAL;
	if (!init || !init->protocol_validated ||
	    init->flow.peer_slot != ctx->peer->slot) {
		admit_fail(ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	/* Candidate selection for a validated initiation; never rescues a witness. */
	rule = vpt1_acl_lookup(ctx->cfg->acl, &init->flow);
	if (!rule) {
		admit_fail(ctx, VPT1_R_ACL_DENY, reason);
		return -EACCES;
	}
	if (!bucket_take(&ctx->peer->new_flows, VPT1_NEW_FLOW_RATE, ctx->now_ns)) {
		admit_fail(ctx, VPT1_R_RATE_LIMIT, reason);
		return -EBUSY;
	}
	if (admit_inflight(ctx, reason))
		return -EBUSY;
	ctx->rule_id = rule;
	ctx->authorized = true;
	return 0;
}

int vpt1_admit_error(struct vpt1_admit_ctx *ctx, const struct vpt1_error_ctx *err,
		     enum vpt1_reason *reason)
{
	if (!ctx->locked)
		return -EINVAL;
	/* An associated error needs its own held, validated context. */
	if (!err || !err->held || !err->validated ||
	    err->peer_slot != ctx->peer->slot || err->flow.peer_slot != ctx->peer->slot ||
	    ctx->now_ns < err->created_ns ||
	    ctx->now_ns - err->created_ns >= VPT1_ERROR_RETENTION_NS) {
		admit_fail(ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	if (!vpt1_acl_allows_rule(ctx->cfg->acl, err->rule_id, &err->flow)) {
		admit_fail(ctx, VPT1_R_ACL_DENY, reason);
		return -EACCES;
	}
	if (!bucket_take(&ctx->peer->errors, VPT1_ERROR_RATE, ctx->now_ns)) {
		admit_fail(ctx, VPT1_R_RATE_LIMIT, reason);
		return -EBUSY;
	}
	if (admit_inflight(ctx, reason))
		return -EBUSY;
	ctx->rule_id = err->rule_id;
	ctx->authorized = true;
	return 0;
}

#define VPT1_TCP_RST 0x04 /* TCP flags byte (header offset 13). */
#define VPT1_TCP_ACK 0x10

/* One candidate flow for the stateless ACL; zero means no rule allows it. */
static u32 packet_rule(const struct vpt1_cfg *cfg, u8 slot, const struct vpt1_pkt_info *pi,
		       u8 direction, bool reply)
{
	struct vpt1_acl_flow f = { .peer_slot = slot, .direction = direction,
				   .protocol = pi->proto };

	if (pi->proto == IPPROTO_ICMP) {
		/* Only diagnostic echo is modelled: a request, or the reply to one. */
		if (pi->icmp_code || pi->icmp_type != (reply ? ICMP_ECHOREPLY : ICMP_ECHO))
			return 0;
		f.icmp_type = ICMP_ECHO;
	}
	if (reply) {
		f.source = pi->dst;
		f.destination = pi->src;
		f.source_port = pi->dport;
		f.destination_port = pi->sport;
	} else {
		f.source = pi->src;
		f.destination = pi->dst;
		f.source_port = pi->sport;
		f.destination_port = pi->dport;
	}
	return vpt1_acl_lookup(cfg->acl, &f);
}

int vpt1_admit_size(struct vpt1_admit_ctx *ctx, unsigned int length,
		    enum vpt1_reason *reason)
{
	u16 cap;

	if (!ctx || !ctx->locked)
		return -EINVAL;
	cap = vpt1_ip_inner_cap(ctx->cfg->peers[ctx->peer->slot].pmtu);
	if (!cap || length < 20 || length > cap) {
		admit_fail(ctx, VPT1_R_SIZE, reason);
		return -EMSGSIZE;
	}
	return 0;
}

static int admit_packet_policy(struct vpt1_admit_ctx *ctx, const struct vpt1_pkt_info *pi,
			       bool reserved, enum vpt1_reason *reason)
{
	const u8 slot = ctx->peer->slot;
	const bool tx = ctx->dir == VPT1_DIR_TX;
	bool reply = false;
	u32 rule;

	if (!ctx->locked)
		return -EINVAL;
	/* An initiation in the direction of travel, or a reply to one the other way. */
	rule = packet_rule(ctx->cfg, slot, pi,
			   tx ? VPT1_ACL_LOCAL_TO_PEER : VPT1_ACL_PEER_TO_LOCAL, false);
	if (!rule) {
		/*
		 * The reply path of a TCP flow carries an ACK (SYN-ACK and everything after it) or
		 * a RST. A bare SYN, or a segment with neither, is the service host starting a
		 * connection of its own from its service port: never admitted here, however its
		 * ports line up with an initiation's reversed tuple.
		 */
		if (pi->proto != IPPROTO_TCP ||
		    (pi->tcp_flags & (VPT1_TCP_ACK | VPT1_TCP_RST))) {
			rule = packet_rule(ctx->cfg, slot, pi,
					   tx ? VPT1_ACL_PEER_TO_LOCAL : VPT1_ACL_LOCAL_TO_PEER,
					   true);
			reply = true;
		}
	}
	if (!rule) {
		admit_fail(ctx, VPT1_R_ACL_DENY, reason);
		return -EACCES;
	}
	if (!reserved && admit_inflight(ctx, reason))
		return -EBUSY;
	ctx->rule_id = rule;
	ctx->reply = reply;
	ctx->authorized = true;
	return 0;
}

int vpt1_admit_packet(struct vpt1_admit_ctx *ctx, const struct vpt1_pkt_info *pi,
		      enum vpt1_reason *reason)
{
	return admit_packet_policy(ctx, pi, false, reason);
}

int vpt1_admit_control_begin(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
			     struct vpt1_generation *gen, u64 now_ns,
			     struct vpt1_admit_ctx *ctx, enum vpt1_reason *reason)
{
	int err = vpt1_admit_begin(peer, held, VPT1_DIR_TX, gen, now_ns, ctx, reason);

	if (err)
		return err;
	if (admit_inflight(ctx, reason))
		return -EBUSY;
	ctx->authorized = true;
	return 0;
}

int vpt1_admit_control_recheck(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
			       struct vpt1_generation *gen, u64 now_ns,
			       enum vpt1_reason *reason)
{
	struct vpt1_admit_ctx ctx;
	int err = admit_begin(peer, held, VPT1_DIR_TX, gen, now_ns, &ctx, true, reason);

	if (err)
		return err;
	if (!atomic_read(&peer->inflight)) {
		admit_fail(&ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	vpt1_admit_abort(&ctx);
	return 0;
}

int vpt1_admit_tx_recheck(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
			  struct vpt1_generation *gen, const struct vpt1_pkt_info *pi,
			  unsigned int length, u64 now_ns, enum vpt1_reason *reason)
{
	struct vpt1_admit_ctx ctx;
	int err;

	err = admit_begin(peer, held, VPT1_DIR_TX, gen, now_ns, &ctx, true, reason);
	if (err)
		return err;
	if (!atomic_read(&peer->inflight)) {
		admit_fail(&ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	err = vpt1_admit_size(&ctx, length, reason);
	if (!err)
		err = admit_packet_policy(&ctx, pi, true, reason);
	/* The caller's original reservation remains counted until output or drop.
	 * Unlock this final policy cut without creating another reservation.
	 */
	if (!err)
		vpt1_admit_abort(&ctx);
	return err;
}

int vpt1_admit_tx_recheck_error(struct vpt1_peer *peer, const struct vpt1_incarnation *held,
				struct vpt1_generation *gen, const struct vpt1_error_ctx *err,
				unsigned int length, u64 now_ns, enum vpt1_reason *reason)
{
	struct vpt1_admit_ctx ctx;
	int ret;

	ret = admit_begin(peer, held, VPT1_DIR_TX, gen, now_ns, &ctx, true, reason);
	if (ret)
		return ret;
	if (!atomic_read(&peer->inflight)) {
		admit_fail(&ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	ret = vpt1_admit_size(&ctx, length, reason);
	if (ret)
		return ret;
	/* The same context and CURRENT saved-rule check as admit_error(), with no rate token. */
	if (!err || !err->held || !err->validated || err->peer_slot != peer->slot ||
	    now_ns < err->created_ns || now_ns - err->created_ns >= VPT1_ERROR_RETENTION_NS) {
		admit_fail(&ctx, VPT1_R_BAD_CONTEXT, reason);
		return -EINVAL;
	}
	if (!vpt1_acl_allows_rule(ctx.cfg->acl, err->rule_id, &err->flow)) {
		admit_fail(&ctx, VPT1_R_ACL_DENY, reason);
		return -EACCES;
	}
	vpt1_admit_abort(&ctx);
	return 0;
}

void vpt1_admit_handoff(struct vpt1_admit_ctx *ctx)
{
	if (WARN_ON_ONCE(!ctx->locked || !ctx->authorized))
		return;
	/* The linearization point: count the packet in flight, drop the lock. */
	atomic_inc(&ctx->peer->inflight);
	peer_unlock_bh(ctx->peer);
	ctx->locked = false;
#ifdef VPT1_DEBUG
	if (vpt1_dbg_post_handoff_us)
		udelay(vpt1_dbg_post_handoff_us);
#endif
}

void vpt1_admit_abort(struct vpt1_admit_ctx *ctx)
{
	if (ctx->locked) {
		peer_unlock_bh(ctx->peer);
		ctx->locked = false;
	}
	ctx->authorized = false;
}

void vpt1_admit_done(struct vpt1_peer *peer)
{
	if (WARN_ON_ONCE(atomic_dec_return(&peer->inflight) < 0))
		wake_up_var(&peer->inflight);
	else if (!atomic_read(&peer->inflight))
		wake_up_var(&peer->inflight);
}
