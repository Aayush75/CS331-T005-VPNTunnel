// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/ip.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/time64.h>
#include <linux/unaligned.h>
#include <net/checksum.h>
#include <net/inet_frag.h>
#include <net/inet_ecn.h>
#include <net/ip.h>
#include "vpt1_fragment.h"

#define HEADER_LEN 20U
#define MAX_PAYLOAD (65535U - HEADER_LEN)
#define LIFETIME_NS (5ULL * NSEC_PER_SEC)

struct frag_piece {
	u8 *bytes;
	u32 offset;
	u32 length;
};

struct frag_set {
	void *context;
	u64 created_ns;
	struct frag_piece pieces[VPT1_FRAG_MAX_PIECES];
	__be32 source;
	__be32 destination;
	u32 payload;
	u32 final_end; /* Zero until a nonempty final fragment arrives. */
	u32 max_end;
	u32 max_nonfinal;
	u32 max_fragment_size;
	u32 max_df_size;
	__be16 id;
	u8 header[HEADER_LEN]; /* Replaced by offset-zero header, regardless of order. */
	u8 protocol;
	u8 ingress;
	u8 count;
	u8 ttl;
	u8 dscp;
	u8 ecn_mask;
	bool first;
};

struct vpt1_frag_cache {
	/* Serializes cache state; process context only, with no peer lock held. */
	struct mutex lock;
	struct vpt1_frag_ops ops;
	void *budget_owner;
	struct frag_set sets[VPT1_FRAG_MAX_SETS];
	u64 last_ns;
	size_t charge;
	u32 payload;
	u32 active;
	u32 results;
	bool closed;
};

struct vpt1_frag_result {
	struct vpt1_frag_cache *owner;
	void *context;
	size_t charge;
	u32 length;
	u32 max_fragment_size;
	u8 ingress;
	bool frag_pmtu;
	u8 packet[] __counted_by(length);
};

struct frag_header {
	__be32 source;
	__be32 destination;
	u32 offset;
	u32 length;
	__be16 id;
	u16 flags;
	u8 protocol;
	u8 ttl;
	u8 tos;
};

/* One call expires at most 16 old sets and may reject one newly created set. */
struct pending_puts {
	void *contexts[VPT1_FRAG_MAX_SETS + 1];
	unsigned int count;
};

static void put_pending(struct vpt1_frag_cache *cache, struct pending_puts *pending)
{
	void (*put)(void *) = cache->ops.put;
	unsigned int i;

	/* The caller's owner reference keeps cache alive across reentrant callbacks. */
	for (i = 0; i < pending->count; i++)
		put(pending->contexts[i]);
}

size_t vpt1_frag_cache_charge(void)
{
	return kmalloc_size_roundup(sizeof(struct vpt1_frag_cache));
}

struct vpt1_frag_cache *
vpt1_frag_create(const struct vpt1_frag_ops *ops, void *budget_owner)
{
	struct vpt1_frag_cache *cache;
	size_t charge = vpt1_frag_cache_charge();

	if (!ops || !ops->get || !ops->put || !ops->reserve || !ops->release)
		return ERR_PTR(-EINVAL);
	if (charge > VPT1_FRAG_MAX_CHARGE || !ops->reserve(budget_owner, charge))
		return ERR_PTR(-ENOSPC);
	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache) {
		ops->release(budget_owner, charge);
		return ERR_PTR(-ENOMEM);
	}
	mutex_init(&cache->lock);
	cache->ops = *ops;
	cache->budget_owner = budget_owner;
	cache->charge = charge;
	return cache;
}

static bool reserve(struct vpt1_frag_cache *cache, size_t charge)
{
	if (charge > VPT1_FRAG_MAX_CHARGE - cache->charge ||
	    !cache->ops.reserve(cache->budget_owner, charge))
		return false;
	cache->charge += charge;
	return true;
}

static void release(struct vpt1_frag_cache *cache, size_t charge)
{
	cache->charge -= charge;
	cache->ops.release(cache->budget_owner, charge);
}

static void drop_set(struct vpt1_frag_cache *cache, struct frag_set *set,
		     struct pending_puts *pending)
{
	unsigned int i;

	for (i = 0; i < set->count; i++) {
		kfree(set->pieces[i].bytes);
		release(cache, kmalloc_size_roundup(set->pieces[i].length));
	}
	cache->payload -= set->payload;
	cache->active--;
	if (set->context)
		pending->contexts[pending->count++] = set->context;
	memset(set, 0, sizeof(*set));
}

static int expire_locked(struct vpt1_frag_cache *cache, u64 now_ns,
			 struct pending_puts *pending)
{
	unsigned int i;
	int expired = 0;

	/* Workers may sample before locking and acquire this mutex in reverse order. */
	now_ns = max(now_ns, cache->last_ns);
	cache->last_ns = now_ns;
	for (i = 0; i < VPT1_FRAG_MAX_SETS; i++) {
		struct frag_set *set = &cache->sets[i];

		if (set->context && now_ns - set->created_ns >= LIFETIME_NS) {
			drop_set(cache, set, pending);
			expired++;
		}
	}
	return expired;
}

static int read_header(const u8 *packet, size_t length, struct frag_header *header)
{
	if (!packet || length < HEADER_LEN || length > 65535 || packet[0] != 0x45 ||
	    get_unaligned_be16(packet + 2) != length ||
	    csum_fold(csum_partial(packet, HEADER_LEN, 0)))
		return -EINVAL;
	if (packet[9] != IPPROTO_TCP && packet[9] != IPPROTO_UDP &&
	    packet[9] != IPPROTO_ICMP)
		return -EPROTONOSUPPORT;
	memcpy(&header->source, packet + 12, sizeof(header->source));
	memcpy(&header->destination, packet + 16, sizeof(header->destination));
	memcpy(&header->id, packet + 4, sizeof(header->id));
	header->flags = get_unaligned_be16(packet + 6);
	header->offset = (header->flags & IP_OFFSET) * 8U;
	header->length = length - HEADER_LEN;
	header->protocol = packet[9];
	header->ttl = packet[8];
	header->tos = packet[1];
	return 0;
}

static bool geometry_valid(const struct frag_header *header)
{
	if (!header->length || !header->ttl ||
	    !(header->flags & (IP_MF | IP_OFFSET)) ||
	    (header->flags & IP_CE) ||
	    header->offset > MAX_PAYLOAD ||
	    header->length > MAX_PAYLOAD - header->offset)
		return false;
	return !(header->flags & IP_MF) || !(header->length % 8);
}

static bool matches(const struct frag_set *set, const struct vpt1_frag_scope *scope,
		    const struct frag_header *header)
{
	return set->context == scope->context && set->ingress == scope->ingress &&
	       set->source == header->source && set->destination == header->destination &&
	       set->id == header->id && set->protocol == header->protocol;
}

static bool consistent(const struct frag_set *set, const struct frag_header *header)
{
	u32 end = header->offset + header->length;
	u8 ecn = set->ecn_mask | BIT(header->tos & INET_ECN_MASK);
	unsigned int i;

	if (header->ttl != set->ttl || (header->tos & ~INET_ECN_MASK) != set->dscp ||
	    ip_frag_ecn_table[ecn] == 0xff)
		return false;
	if (set->final_end) {
		if (end > set->final_end ||
		    ((header->flags & IP_MF) && end == set->final_end))
			return false;
	}
	if (!(header->flags & IP_MF) &&
	    ((set->final_end && end != set->final_end) ||
	     end < set->max_end || end <= set->max_nonfinal))
		return false;
	for (i = 0; i < set->count; i++) {
		const struct frag_piece *piece = &set->pieces[i];

		if (header->offset < piece->offset + piece->length && piece->offset < end)
			return false;
	}
	return true;
}

static int assemble(struct vpt1_frag_cache *cache, struct frag_set *set,
		    struct vpt1_frag_result **result, struct pending_puts *pending)
{
	struct vpt1_frag_result *packet;
	u32 length = HEADER_LEN + set->final_end;
	size_t charge;
	__sum16 checksum;
	unsigned int i;

	charge = kmalloc_size_roundup(struct_size(packet, packet, length));
	if (!reserve(cache, charge))
		return -ENOSPC;
	packet = kmalloc(struct_size(packet, packet, length), GFP_KERNEL);
	if (!packet) {
		release(cache, charge);
		return -ENOMEM;
	}
	packet->length = length;
	packet->owner = cache;
	packet->context = set->context;
	packet->charge = charge;
	packet->ingress = set->ingress;
	packet->max_fragment_size = set->max_fragment_size;
	packet->frag_pmtu = set->max_df_size == set->max_fragment_size;
	memcpy(packet->packet, set->header, HEADER_LEN);
	for (i = 0; i < set->count; i++) {
		const struct frag_piece *piece = &set->pieces[i];

		memcpy(packet->packet + HEADER_LEN + piece->offset, piece->bytes, piece->length);
	}
	packet->packet[1] |= ip_frag_ecn_table[set->ecn_mask];
	put_unaligned_be16(length, packet->packet + 2);
	put_unaligned_be16(packet->frag_pmtu ? IP_DF : 0, packet->packet + 6);
	put_unaligned_be16(0, packet->packet + 10);
	checksum = csum_fold(csum_partial(packet->packet, HEADER_LEN, 0));
	memcpy(packet->packet + 10, &checksum, sizeof(checksum));
	/* Transfer the held context to the result; drop_set releases only the pieces. */
	set->context = NULL;
	drop_set(cache, set, pending);
	cache->results++;
	*result = packet;
	return 1;
}

int vpt1_frag_submit(struct vpt1_frag_cache *cache,
		     const struct vpt1_frag_scope *scope, const u8 *packet,
		     size_t length, u64 now_ns, struct vpt1_frag_result **result)
{
	struct pending_puts pending = {};
	struct frag_set *set = NULL, *unused = NULL;
	struct frag_header header;
	struct frag_piece *piece;
	size_t charge;
	unsigned int i;
	u32 end;
	int err;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!cache || !scope || !scope->context ||
	    (scope->ingress != VPT1_FRAG_LOCAL && scope->ingress != VPT1_FRAG_TUNNEL))
		return -EINVAL;
	err = read_header(packet, length, &header);
	if (err)
		return err;
	mutex_lock(&cache->lock);
	if (cache->closed) {
		err = -ESHUTDOWN;
		goto out;
	}
	expire_locked(cache, now_ns, &pending);
	now_ns = cache->last_ns;
	for (i = 0; i < VPT1_FRAG_MAX_SETS; i++) {
		if (!cache->sets[i].context)
			unused = &cache->sets[i];
		else if (matches(&cache->sets[i], scope, &header))
			set = &cache->sets[i];
	}
	err = -EINVAL;
	if (!geometry_valid(&header))
		goto reject;
	if (set && !consistent(set, &header))
		goto reject;
	err = -E2BIG;
	if ((set && set->count == VPT1_FRAG_MAX_PIECES) ||
	    header.length > VPT1_FRAG_MAX_PAYLOAD - cache->payload)
		goto reject;
	if (!set) {
		if (!unused) {
			err = -ENOSPC;
			goto out;
		}
		if (!cache->ops.get(scope->context)) {
			err = -ESTALE;
			goto out;
		}
		set = unused;
		set->context = scope->context;
		set->ingress = scope->ingress;
		set->created_ns = now_ns;
		set->source = header.source;
		set->destination = header.destination;
		set->id = header.id;
		set->protocol = header.protocol;
		set->ttl = header.ttl;
		set->dscp = header.tos & ~INET_ECN_MASK;
		cache->active++;
	}
	charge = kmalloc_size_roundup(header.length);
	err = -ENOSPC;
	if (!reserve(cache, charge))
		goto reject;
	piece = &set->pieces[set->count];
	piece->bytes = kmalloc(header.length, GFP_KERNEL);
	if (!piece->bytes) {
		release(cache, charge);
		err = -ENOMEM;
		goto reject;
	}
	memcpy(piece->bytes, packet + HEADER_LEN, header.length);
	piece->length = header.length;
	piece->offset = header.offset;
	set->count++;
	set->payload += header.length;
	cache->payload += header.length;
	set->ecn_mask |= BIT(header.tos & INET_ECN_MASK);
	end = header.offset + header.length;
	set->max_end = max(set->max_end, end);
	set->max_fragment_size = max_t(u32, set->max_fragment_size, length);
	if (header.flags & IP_DF)
		set->max_df_size = max_t(u32, set->max_df_size, length);
	if (header.flags & IP_MF)
		set->max_nonfinal = max(set->max_nonfinal, end);
	else
		set->final_end = end;
	if (!header.offset) {
		memcpy(set->header, packet, HEADER_LEN);
		set->first = true;
	}
	err = 0;
	if (set->first && set->final_end && set->payload == set->final_end) {
		err = assemble(cache, set, result, &pending);
		if (err < 0)
			goto reject;
	}
	goto out;
reject:
	if (set)
		drop_set(cache, set, &pending);
out:
	mutex_unlock(&cache->lock);
	put_pending(cache, &pending);
	return err;
}

int vpt1_frag_expire(struct vpt1_frag_cache *cache, u64 now_ns)
{
	struct pending_puts pending = {};
	int err;

	if (!cache)
		return -EINVAL;
	mutex_lock(&cache->lock);
	err = expire_locked(cache, now_ns, &pending);
	mutex_unlock(&cache->lock);
	put_pending(cache, &pending);
	return err;
}

void vpt1_frag_flush_context(struct vpt1_frag_cache *cache, void *context)
{
	struct pending_puts pending = {};
	unsigned int i;

	if (!cache)
		return;
	mutex_lock(&cache->lock);
	for (i = 0; i < VPT1_FRAG_MAX_SETS; i++)
		if (cache->sets[i].context == context && context)
			drop_set(cache, &cache->sets[i], &pending);
	mutex_unlock(&cache->lock);
	put_pending(cache, &pending);
}

void vpt1_frag_close(struct vpt1_frag_cache *cache)
{
	struct pending_puts pending = {};
	unsigned int i;

	if (!cache)
		return;
	mutex_lock(&cache->lock);
	cache->closed = true;
	for (i = 0; i < VPT1_FRAG_MAX_SETS; i++)
		if (cache->sets[i].context)
			drop_set(cache, &cache->sets[i], &pending);
	mutex_unlock(&cache->lock);
	put_pending(cache, &pending);
}

void vpt1_frag_usage(struct vpt1_frag_cache *cache, struct vpt1_frag_usage *usage)
{
	if (!usage)
		return;
	memset(usage, 0, sizeof(*usage));
	usage->closed = true;
	if (!cache)
		return;
	mutex_lock(&cache->lock);
	usage->charge = cache->charge;
	usage->payload = cache->payload;
	usage->sets = cache->active;
	usage->results = cache->results;
	usage->closed = cache->closed;
	mutex_unlock(&cache->lock);
}

void vpt1_frag_view(const struct vpt1_frag_result *result, struct vpt1_frag_view *view)
{
	if (!view)
		return;
	memset(view, 0, sizeof(*view));
	if (!result)
		return;
	view->packet = result->packet;
	view->length = result->length;
	view->context = result->context;
	view->ingress = result->ingress;
	view->max_fragment_size = result->max_fragment_size;
	view->frag_pmtu = result->frag_pmtu;
}

void vpt1_frag_result_free(struct vpt1_frag_result *result)
{
	struct vpt1_frag_cache *cache;
	void *context;

	if (!result)
		return;
	cache = result->owner;
	context = result->context;
	mutex_lock(&cache->lock);
	release(cache, result->charge);
	cache->results--;
	kfree(result);
	mutex_unlock(&cache->lock);
	cache->ops.put(context);
}

int vpt1_frag_destroy(struct vpt1_frag_cache *cache)
{
	if (!cache)
		return -EINVAL;
	/* Caller has already joined all users; no new operation may race destruction. */
	mutex_lock(&cache->lock);
	if (!cache->closed || cache->active || cache->results) {
		mutex_unlock(&cache->lock);
		return -EBUSY;
	}
	mutex_unlock(&cache->lock);
	cache->ops.release(cache->budget_owner, cache->charge);
	mutex_destroy(&cache->lock);
	kfree(cache);
	return 0;
}
