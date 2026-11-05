// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include "vpt1_zone_pool.h"

struct vpt1_zone_pool {
	u64 *serials;
	u8 *states;
	u64 next_serial;
	u32 count;
	u32 cursor;
	u32 occupied;
	u16 first;
};

size_t vpt1_zone_charge_bound(u16 first, u16 last)
{
	struct vpt1_zone_pool *pool;
	u32 count;

	if (!first || last < first)
		return 0;
	count = (u32)last - first + 1;
	return kmalloc_size_roundup(sizeof(*pool)) +
	       kmalloc_size_roundup(array_size(count, sizeof(u64))) +
	       kmalloc_size_roundup(array_size(count, sizeof(u8)));
}

struct vpt1_zone_pool *vpt1_zone_create(u16 first, u16 last)
{
	struct vpt1_zone_pool *pool;
	u32 count;

	if (!first || last < first)
		return ERR_PTR(-EINVAL);
	count = (u32)last - first + 1;
	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);
	pool->states = kcalloc(count, sizeof(*pool->states), GFP_KERNEL);
	pool->serials = kcalloc(count, sizeof(*pool->serials), GFP_KERNEL);
	if (!pool->states || !pool->serials) {
		kfree(pool->serials);
		kfree(pool->states);
		kfree(pool);
		return ERR_PTR(-ENOMEM);
	}
	pool->count = count;
	pool->first = first;
	return pool;
}

int vpt1_zone_destroy(struct vpt1_zone_pool *pool)
{
	if (!pool)
		return -EINVAL;
	if (pool->occupied)
		return -EBUSY;
	kfree(pool->serials);
	kfree(pool->states);
	kfree(pool);
	return 0;
}

static int index_of(const struct vpt1_zone_pool *pool,
		    const struct vpt1_zone_token *token)
{
	u32 index;

	if (!pool || !token || token->zone < pool->first ||
	    (u32)token->zone - pool->first >= pool->count)
		return -EINVAL;
	index = token->zone - pool->first;
	if (!token->serial || pool->serials[index] != token->serial)
		return -ESTALE;
	return index;
}

int vpt1_zone_phase(const struct vpt1_zone_pool *pool,
		    const struct vpt1_zone_token *token)
{
	int index = index_of(pool, token);

	return index < 0 ? index : pool->states[index];
}

int vpt1_zone_reserve(struct vpt1_zone_pool *pool, struct vpt1_zone_token *token)
{
	u32 index, searched;

	if (!pool || !token)
		return -EINVAL;
	if (pool->occupied == pool->count)
		return -ENOSPC;
	if (pool->next_serial == U64_MAX)
		return -EOVERFLOW;
	index = pool->cursor;
	for (searched = 0; searched < pool->count; searched++) {
		if (pool->states[index] == VPT1_ZONE_FREE) {
			pool->states[index] = VPT1_ZONE_RESERVED;
			pool->occupied++;
			pool->serials[index] = ++pool->next_serial;
			token->serial = pool->serials[index];
			token->zone = pool->first + index;
			pool->cursor = index + 1 == pool->count ? 0 : index + 1;
			return 0;
		}
		index = index + 1 == pool->count ? 0 : index + 1;
	}
	return -ENOSPC;
}

static int transition(struct vpt1_zone_pool *pool,
		      const struct vpt1_zone_token *token, u8 from, u8 to)
{
	int index = index_of(pool, token);

	if (index < 0)
		return index;
	if (pool->states[index] != from)
		return -EBUSY;
	pool->states[index] = to;
	return 0;
}

int vpt1_zone_confirm_clean(struct vpt1_zone_pool *pool,
			    const struct vpt1_zone_token *token)
{
	return transition(pool, token, VPT1_ZONE_RESERVED, VPT1_ZONE_ACTIVE);
}

int vpt1_zone_retire(struct vpt1_zone_pool *pool, const struct vpt1_zone_token *token)
{
	int index = index_of(pool, token);

	if (index < 0)
		return index;
	if (pool->states[index] != VPT1_ZONE_RESERVED &&
	    pool->states[index] != VPT1_ZONE_ACTIVE)
		return -EBUSY;
	pool->states[index] = VPT1_ZONE_DRAINING;
	return 0;
}

int vpt1_zone_drained(struct vpt1_zone_pool *pool, const struct vpt1_zone_token *token)
{
	return transition(pool, token, VPT1_ZONE_DRAINING, VPT1_ZONE_PURGE_PENDING);
}

int vpt1_zone_purged(struct vpt1_zone_pool *pool, const struct vpt1_zone_token *token)
{
	int index = index_of(pool, token);

	if (index < 0)
		return index;
	if (pool->states[index] != VPT1_ZONE_PURGE_PENDING)
		return -EBUSY;
	pool->states[index] = VPT1_ZONE_FREE;
	pool->serials[index] = 0;
	pool->occupied--;
	return 0;
}
