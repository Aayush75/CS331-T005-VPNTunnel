/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_ZONE_POOL_H
#define VPT1_ZONE_POOL_H

#include <linux/types.h>

enum vpt1_zone_phase {
	VPT1_ZONE_FREE,
	VPT1_ZONE_RESERVED,
	VPT1_ZONE_ACTIVE,
	VPT1_ZONE_DRAINING,
	VPT1_ZONE_PURGE_PENDING,
};

struct vpt1_zone_pool;

/* Pool-local reservation identity, not a peer incarnation or packet authority. */
struct vpt1_zone_token {
	u64 serial;
	u16 zone;
};

/*
 * One value/resource pool owned by the shared gateway/netns configuration owner;
 * not a peer, namespace or conntrack lifetime implementation. Caller serializes
 * every operation under its configuration writer lock, in process context only.
 * Reserve charge_bound() in the shared memory budget before allocating.
 * create returns ERR_PTR(-EINVAL/-ENOMEM); first..last must be nonzero u16 zones.
 * Pool destruction fails -EBUSY while any reservation/active/retired zone exists.
 * Caller must keep pool storage alive until its queued drain/purge events finish.
 */
size_t vpt1_zone_charge_bound(u16 first, u16 last);
struct vpt1_zone_pool *vpt1_zone_create(u16 first, u16 last);
int vpt1_zone_destroy(struct vpt1_zone_pool *pool);

/* reserve never grants activation or assumes pre-existing CT entries are clean.
 * After actual kernel conntrack clean/purge verification, confirm_clean marks
 * the reserved zone active. Both directions still need the same held continuity
 * context, nonzero template assignment and current packet admission externally.
 */
int vpt1_zone_reserve(struct vpt1_zone_pool *pool, struct vpt1_zone_token *token);
int vpt1_zone_confirm_clean(struct vpt1_zone_pool *pool,
			    const struct vpt1_zone_token *token);

/*
 * Caller closes eligibility FIRST, then retires. Only after every old witness /
 * queued/CT-template reference drains may it report drained. Only a successful
 * project-conntrack purge may report purged and release the number for reuse.
 * Failure leaves the zone quarantined; never call these on optimistic success.
 * These functions validate order, not the truth of external completion events.
 * No packets, references, locks, callbacks or namespace pointers are retained.
 */
int vpt1_zone_retire(struct vpt1_zone_pool *pool, const struct vpt1_zone_token *token);
int vpt1_zone_drained(struct vpt1_zone_pool *pool, const struct vpt1_zone_token *token);
int vpt1_zone_purged(struct vpt1_zone_pool *pool, const struct vpt1_zone_token *token);
int vpt1_zone_phase(const struct vpt1_zone_pool *pool,
		    const struct vpt1_zone_token *token);

#endif /* VPT1_ZONE_POOL_H */
