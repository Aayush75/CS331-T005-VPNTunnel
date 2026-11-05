/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_FRAGMENT_H
#define VPT1_FRAGMENT_H

#include <linux/types.h>

#define VPT1_FRAG_MAX_SETS 16U
#define VPT1_FRAG_MAX_PIECES 64U
#define VPT1_FRAG_MAX_PAYLOAD (256U * 1024U)
#define VPT1_FRAG_MAX_CHARGE (512U * 1024U)

enum vpt1_frag_ingress {
	VPT1_FRAG_LOCAL = 1,
	VPT1_FRAG_TUNNEL = 2,
};

struct vpt1_frag_cache;
struct vpt1_frag_result;

/* Adapters to the ONE shared runtime, not another context/budget implementation.
 * get/put pin the canonical namespace/peer-incarnation/continuity context.
 * reserve/release cover the shared gateway/host allocation gates, in bytes.
 * get/reserve/release run under the cache mutex: nonblocking, non-reentrant,
 * and never take peer/config locks. put ALWAYS runs after releasing that mutex;
 * it may synchronously flush/close/query this cache. The owner must still pin
 * cache/budget lifetime until the outer call and all its callbacks return;
 * put must not destroy a cache whose callers have not drained.
 * Their code and budget_owner must outlive the cache and every returned result.
 */
struct vpt1_frag_ops {
	bool (*get)(void *context);
	void (*put)(void *context);
	bool (*reserve)(void *budget_owner, size_t charge);
	void (*release)(void *budget_owner, size_t charge);
};

struct vpt1_frag_scope {
	void *context; /* Borrowed live canonical continuity reference, NOT an skb mark. */
	u8 ingress;
};

struct vpt1_frag_view {
	const u8 *packet;
	void *context; /* Borrowed from result until result_free; not output authority. */
	u32 length;
	u32 max_fragment_size;
	u8 ingress;
	bool frag_pmtu; /* Adapter must preserve Linux's DF-fragment PMTU constraint. */
};

struct vpt1_frag_usage {
	size_t charge; /* Includes cache, retained bytes and outstanding result buffers. */
	u32 payload;
	u32 sets;
	u32 results;
	bool closed;
};

/* Process context only for ALL cache/result operations: they may sleep.
 * One cache per canonical gateway/netns PEER, shared by both ingress roles and
 * all its continuity contexts. Private mutex serializes submit/GC/close/free;
 * never call while holding a packet/peer spinlock or runtime configuration lock. Dispatch from
 * the -450 hook to AY2's charged bounded worker BEFORE shared defrag/conntrack.
 * This component does not install that hook/queue or implement authentication.
 * create reserves its full bucket charge before allocation; errors are ERR_PTR.
 */
size_t vpt1_frag_cache_charge(void);
struct vpt1_frag_cache *
vpt1_frag_create(const struct vpt1_frag_ops *ops, void *budget_owner);

/* Borrow a stable linear IPv4 fragment for this call; never consumes input.
 * Caller MUST first authenticate/source-check and validate current scope,
 * ingress and generation eligibility. Nonlinear/GSO skb adaptation is external.
 * now_ns is suspend-inclusive boot time, never sender/wall-clock time. Samples
 * taken before locking may arrive out of order; clamp to the cache watermark.
 * Older samples never move time backward or extend an existing set's lifetime.
 * Returns 0 queued, 1 complete, negative errno rejected; *result is NULL unless
 * complete. A completed result owns the context reference and charged bytes.
 * No output permission is minted: full transport/checksum/current-policy and
 * final admission are mandatory after normalization, before reinjection/output.
 */
int vpt1_frag_submit(struct vpt1_frag_cache *cache,
		     const struct vpt1_frag_scope *scope, const u8 *packet,
		     size_t length, u64 now_ns, struct vpt1_frag_result **result);
int vpt1_frag_expire(struct vpt1_frag_cache *cache, u64 now_ns);
/* The adapter MUST schedule expire periodically even when no packets arrive. */
void vpt1_frag_flush_context(struct vpt1_frag_cache *cache, void *context);
void vpt1_frag_close(struct vpt1_frag_cache *cache);
void vpt1_frag_usage(struct vpt1_frag_cache *cache, struct vpt1_frag_usage *usage);
void vpt1_frag_view(const struct vpt1_frag_result *result, struct vpt1_frag_view *view);
void vpt1_frag_result_free(struct vpt1_frag_result *result);

/* Owner first closes eligibility, closes cache, joins/drains ALL callers and
 * releases results. destroy is process-context and refuses an open/busy cache.
 * Neither close nor flush can revoke already returned bytes: final admission
 * must reject late results after closure/policy/context change. No forced free.
 */
int vpt1_frag_destroy(struct vpt1_frag_cache *cache);

#endif /* VPT1_FRAGMENT_H */
