// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/time64.h>
#include <linux/unaligned.h>
#include <net/ip.h>
#include "vpt1_fragment.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define fragment_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct fragment_test {
	unsigned int assertions;
	unsigned int failures;
};

static void fragment_expect(struct fragment_test *t, bool condition,
			    const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_fragment_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) fragment_expect(t, condition, __func__, __LINE__)
#endif

/* Test-only holders exercise real reference counts and a shared atomic budget.
 * They do not authenticate packets or stand in for an implemented AY2 runtime.
 */
struct test_context {
	refcount_t refs;
	bool available;
	struct vpt1_frag_cache *reenter;
	atomic_t puts;
	bool close_on_put;
};

struct test_budget {
	atomic_long_t used;
	atomic_long_t peak;
	size_t limit;
};

struct test_env {
	struct vpt1_frag_cache *cache;
	struct test_budget budget;
	struct test_context context;
	struct vpt1_frag_scope scope;
};

static bool context_get(void *opaque)
{
	struct test_context *context = opaque;

	return context->available && refcount_inc_not_zero(&context->refs);
}

static void context_put(void *opaque)
{
	struct test_context *context = opaque;
	struct vpt1_frag_usage usage;

	refcount_dec(&context->refs);
	atomic_inc(&context->puts);
	if (context->reenter) {
		vpt1_frag_usage(context->reenter, &usage);
		vpt1_frag_flush_context(context->reenter, context);
		if (context->close_on_put)
			vpt1_frag_close(context->reenter);
	}
}

static bool budget_reserve(void *opaque, size_t charge)
{
	struct test_budget *budget = opaque;
	long before, peak, total;

	do {
		before = atomic_long_read(&budget->used);
		if (before < 0 || before > budget->limit || charge > budget->limit - before)
			return false;
	} while (atomic_long_cmpxchg(&budget->used, before, before + charge) != before);
	total = before + charge;
	do {
		peak = atomic_long_read(&budget->peak);
		if (peak >= total)
			break;
	} while (atomic_long_cmpxchg(&budget->peak, peak, total) != peak);
	return true;
}

static void budget_release(void *opaque, size_t charge)
{
	struct test_budget *budget = opaque;

	atomic_long_sub(charge, &budget->used);
}

static const struct vpt1_frag_ops ops = {
	.get = context_get,
	.put = context_put,
	.reserve = budget_reserve,
	.release = budget_release,
};

static bool start(struct fragment_test *t, struct test_env *env)
{
	memset(env, 0, sizeof(*env));
	atomic_long_set(&env->budget.used, 0);
	atomic_long_set(&env->budget.peak, 0);
	env->budget.limit = VPT1_FRAG_MAX_CHARGE;
	refcount_set(&env->context.refs, 1);
	env->context.available = true;
	env->scope.context = &env->context;
	env->scope.ingress = VPT1_FRAG_TUNNEL;
	env->cache = vpt1_frag_create(&ops, &env->budget);
	EXPECT(t, !IS_ERR(env->cache));
	return !IS_ERR(env->cache);
}

static void finish(struct fragment_test *t, struct test_env *env)
{
	struct vpt1_frag_usage usage;

	vpt1_frag_close(env->cache);
	vpt1_frag_usage(env->cache, &usage);
	EXPECT(t, !usage.sets && !usage.payload && !usage.results && usage.closed);
	EXPECT(t, usage.charge == vpt1_frag_cache_charge());
	EXPECT(t, refcount_read(&env->context.refs) == 1);
	EXPECT(t, !vpt1_frag_destroy(env->cache));
	EXPECT(t, !atomic_long_read(&env->budget.used));
}

/* Independent byte-wise Internet checksum, not the implementation's helper. */
static u16 checksum(const u8 *data, unsigned int length)
{
	u32 sum = 0;
	unsigned int i;

	for (i = 0; i + 1 < length; i += 2)
		sum += (data[i] << 8) | data[i + 1];
	if (length & 1)
		sum += data[length - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static void fix_checksum(u8 *data)
{
	data[10] = 0;
	data[11] = 0;
	put_unaligned_be16(checksum(data, 20), data + 10);
}

static u8 payload_byte(u32 offset)
{
	return offset * 37 + 11;
}

static u8 *fragment(u16 id, u32 offset, u32 length, bool more, u8 tos)
{
	u8 *data;
	u32 i;

	data = kzalloc(20 + length, GFP_KERNEL);
	if (!data)
		return NULL;
	data[0] = 0x45;
	data[1] = tos;
	put_unaligned_be16(20 + length, data + 2);
	put_unaligned_be16(id, data + 4);
	put_unaligned_be16(offset / 8 | (more ? IP_MF : 0), data + 6);
	data[8] = 61;
	data[9] = IPPROTO_UDP;
	data[12] = 10;
	data[13] = 77;
	data[14] = 1;
	data[15] = 10;
	data[16] = 10;
	data[17] = 77;
	data[18] = 2;
	data[19] = 10;
	for (i = 0; i < length; i++)
		data[20 + i] = payload_byte(offset + i);
	fix_checksum(data);
	return data;
}

static int send(struct test_env *env, const struct vpt1_frag_scope *scope,
		u16 id, u32 offset, u32 length, bool more, u8 tos, u64 now,
		struct vpt1_frag_result **result)
{
	u8 *data = fragment(id, offset, length, more, tos);
	int err;

	if (!data)
		return -ENOMEM;
	err = vpt1_frag_submit(env->cache, scope, data, 20 + length, now, result);
	kfree(data);
	return err;
}

static void verify(struct fragment_test *t, struct test_env *env,
		   struct vpt1_frag_result *result, u32 length, u8 tos)
{
	struct vpt1_frag_view view;
	u32 i;

	EXPECT(t, result);
	if (!result)
		return;
	vpt1_frag_view(result, &view);
	EXPECT(t, view.context == env->scope.context && view.ingress == env->scope.ingress);
	EXPECT(t, view.length == 20 + length);
	EXPECT(t, !checksum(view.packet, 20));
	EXPECT(t, view.packet[1] == tos && view.packet[8] == 61);
	EXPECT(t, !get_unaligned_be16(view.packet + 6));
	EXPECT(t, get_unaligned_be16(view.packet + 2) == view.length);
	for (i = 0; i < length; i++)
		EXPECT(t, view.packet[20 + i] == payload_byte(i));
	vpt1_frag_result_free(result);
}

static void reordering_and_output(struct fragment_test *t)
{
	static const u8 order[][4] = {
		{0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1},
		{0, 3, 1, 2}, {0, 3, 2, 1}, {1, 0, 2, 3}, {1, 0, 3, 2},
		{1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0},
		{2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0},
		{2, 3, 0, 1}, {2, 3, 1, 0}, {3, 0, 1, 2}, {3, 0, 2, 1},
		{3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0},
	};
	struct vpt1_frag_result *result;
	unsigned int seed, row, i, part;
	struct test_env env;
	int err;

	if (!start(t, &env))
		return;
	for (seed = 0; seed < 32; seed++) {
		for (row = 0; row < ARRAY_SIZE(order); row++) {
			for (i = 0; i < 4; i++) {
				part = order[row][i];
				err = send(&env, &env.scope, seed, part * 16,
					   part == 3 ? 9 : 16, part != 3, 0x2a, 0, &result);
				EXPECT(t, err == (i == 3 ? 1 : 0));
				if (i != 3)
					EXPECT(t, !result);
			}
			verify(t, &env, result, 57, 0x2a);
		}
	}
	finish(t, &env);
}

static void malformed_and_poisoned_sets(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct test_env env;
	u8 *data;
	unsigned int variant, length;
	int err;

	if (!start(t, &env))
		return;
	for (variant = 0; variant < 12; variant++) {
		data = fragment(1, 0, 16, true, 2);
		EXPECT(t, data);
		if (!data)
			break;
		switch (variant) {
		case 0:
			data[0] = 0x65;
			break;
		case 1:
			data[0] = 0x46; /* Options are unsupported. */
			break;
		case 2:
			data[2] = 0xff;
			break;
		case 3:
			data[3]--;
			break;
		case 4:
			data[6] |= 0x80; /* Reserved flag. */
			break;
		case 5:
			data[8] = 0;
			break;
		case 6:
			data[9] = 99;
			break;
		case 7:
			data[6] = 0; /* Not a fragment. */
			break;
		case 8:
			data[6] = 0x1f;
			data[7] = 0xff;
			break;
		case 9:
			data[10] ^= 1; /* Bad checksum: do not repair. */
			break;
		case 10:
			data[3] = 20; /* Zero payload. */
			break;
		case 11:
			data[3] = 35; /* MF length not divisible by eight. */
			break;
		}
		if (variant != 9)
			fix_checksum(data);
		length = variant == 10 ? 20 : variant == 11 ? 35 : 36;
		err = vpt1_frag_submit(env.cache, &env.scope, data, length, 0, &result);
		EXPECT(t, err < 0);
		EXPECT(t, !result);
		vpt1_frag_usage(env.cache, &usage);
		EXPECT(t, !usage.sets && usage.charge == vpt1_frag_cache_charge());
		kfree(data);
	}
	for (variant = 0; variant < 4; variant++) {
		EXPECT(t, !send(&env, &env.scope, 1, 0, 16, true, 2, 0, &result));
		data = fragment(1, 16, 8, false, 2);
		EXPECT(t, data);
		if (!data)
			break;
		if (variant == 0)
			data[8]--;
		else if (variant == 1)
			data[1] ^= 4;
		else if (variant == 2)
			put_unaligned_be16(1, data + 6); /* Overlap starts at byte eight. */
		else
			put_unaligned_be16(0xffff, data + 6);
		fix_checksum(data);
		EXPECT(t, vpt1_frag_submit(env.cache, &env.scope, data, 28, 0, &result) < 0);
		vpt1_frag_usage(env.cache, &usage);
		EXPECT(t, !usage.sets && !usage.payload);
		EXPECT(t, refcount_read(&env.context.refs) == 1);
		kfree(data);
	}
	/* Two final fragments disagree even when their ranges do not overlap. */
	EXPECT(t, !send(&env, &env.scope, 1, 32, 8, false, 2, 0, &result));
	EXPECT(t, send(&env, &env.scope, 1, 16, 8, false, 2, 0, &result) == -EINVAL);
	/* A non-final fragment may not end at an already declared final end. */
	EXPECT(t, !send(&env, &env.scope, 1, 32, 8, false, 2, 0, &result));
	EXPECT(t, send(&env, &env.scope, 1, 24, 16, true, 2, 0, &result) == -EINVAL);
	finish(t, &env);
}

static void overlap_range_model(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	unsigned int offset, length, byte;
	struct test_env env;
	bool overlaps;
	int err;

	if (!start(t, &env))
		return;
	for (offset = 0; offset <= 64; offset += 8) {
		for (length = 8; length <= 32; length += 8) {
			EXPECT(t, !send(&env, &env.scope, 7, 16, 16, true, 2, 0, &result));
			overlaps = false;
			/* Independent byte-membership oracle includes exact duplicates. */
			for (byte = offset; byte < offset + length; byte++)
				if (byte >= 16 && byte <= 31)
					overlaps = true;
			err = send(&env, &env.scope, 7, offset, length, true, 2, 0, &result);
			EXPECT(t, err == (overlaps ? -EINVAL : 0));
			EXPECT(t, !result);
			vpt1_frag_usage(env.cache, &usage);
			EXPECT(t, usage.sets == (overlaps ? 0 : 1));
			vpt1_frag_flush_context(env.cache, &env.context);
		}
	}
	finish(t, &env);
}

static void ecn_merge_matrix(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_view view;
	unsigned int mask, first, bit, part;
	struct test_env env;
	bool invalid;
	int err;

	if (!start(t, &env))
		return;
	/* Every nonempty ECN mask, each possible offset-zero ECN, reverse arrival. */
	for (mask = 1; mask < 16; mask++) {
		for (first = 0; first < 4; first++) {
			if (!(mask & BIT(first)))
				continue;
			invalid = (mask & 1) && mask != 1;
			part = 1;
			err = 0;
			for (bit = 0; bit < 4; bit++) {
				if (!(mask & BIT(bit)))
					continue;
				err = send(&env, &env.scope, mask, part * 8, 8,
					   part < hweight32(mask), 0x28 | bit, 0, &result);
				part++;
				if (err < 0)
					break;
			}
			if (err >= 0)
				err = send(&env, &env.scope, mask, 0, 8, true,
					   0x28 | first, 0, &result);
			if (invalid) {
				EXPECT(t, err == -EINVAL && !result);
			} else {
				EXPECT(t, err == 1 && result);
				if (result) {
					vpt1_frag_view(result, &view);
					EXPECT(t, (view.packet[1] & 3) == ((mask & 8) ? 3 : first));
					EXPECT(t, (view.packet[1] & 0xfc) == 0x28);
					EXPECT(t, !checksum(view.packet, 20));
					vpt1_frag_result_free(result);
				}
			}
			vpt1_frag_flush_context(env.cache, &env.context);
		}
	}
	finish(t, &env);
}

static void scope_and_invalidation(struct fragment_test *t)
{
	struct test_context other = { .available = true };
	struct vpt1_frag_scope scope;
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct test_env env;

	if (!start(t, &env))
		return;
	refcount_set(&other.refs, 1);
	scope = env.scope;
	scope.context = &other;
	EXPECT(t, !send(&env, &env.scope, 1, 0, 16, true, 2, 0, &result));
	EXPECT(t, !send(&env, &scope, 1, 16, 8, false, 2, 0, &result));
	EXPECT(t, !result);
	/* Same continuity can span a rekey; a different canonical context cannot. */
	EXPECT(t, send(&env, &env.scope, 1, 16, 8, false, 2, 0, &result) == 1);
	verify(t, &env, result, 24, 2);
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, usage.sets == 1 && refcount_read(&other.refs) == 2);
	vpt1_frag_flush_context(env.cache, &other);
	EXPECT(t, refcount_read(&other.refs) == 1);
	scope = env.scope;
	scope.ingress = VPT1_FRAG_LOCAL;
	EXPECT(t, !send(&env, &env.scope, 2, 0, 16, true, 2, 0, &result));
	EXPECT(t, !send(&env, &scope, 2, 16, 8, false, 2, 0, &result));
	EXPECT(t, !result);
	vpt1_frag_flush_context(env.cache, &env.context);
	EXPECT(t, !send(&env, &env.scope, 3, 0, 16, true, 2, 0, &result));
	vpt1_frag_flush_context(env.cache, &env.context);
	EXPECT(t, !send(&env, &env.scope, 3, 16, 8, false, 2, 0, &result));
	EXPECT(t, !result); /* Revoked partial bytes did not survive. */
	vpt1_frag_flush_context(env.cache, &env.context);
	EXPECT(t, !send(&env, &env.scope, 4, 0, 16, true, 2, 0, &result));
	EXPECT(t, send(&env, &env.scope, 4, 16, 8, false, 2, 0, &result) == 1);
	EXPECT(t, vpt1_frag_destroy(env.cache) == -EBUSY);
	vpt1_frag_close(env.cache);
	EXPECT(t, vpt1_frag_destroy(env.cache) == -EBUSY);
	EXPECT(t, refcount_read(&env.context.refs) == 2);
	vpt1_frag_result_free(result);
	EXPECT(t, send(&env, &env.scope, 5, 0, 16, true, 2, 0, &result) == -ESHUTDOWN);
	finish(t, &env);
}

static void fixed_deadlines(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct test_env env;
	u64 second = NSEC_PER_SEC;

	if (!start(t, &env))
		return;
	EXPECT(t, !send(&env, &env.scope, 1, 0, 8, true, 2, 0, &result));
	EXPECT(t, !send(&env, &env.scope, 1, 16, 8, false, 2, 4 * second, &result));
	EXPECT(t, !vpt1_frag_expire(env.cache, 5 * second - 1));
	EXPECT(t, vpt1_frag_expire(env.cache, 5 * second) == 1);
	EXPECT(t, refcount_read(&env.context.refs) == 1);
	EXPECT(t, !send(&env, &env.scope, 1, 8, 8, true, 2, 5 * second, &result));
	EXPECT(t, !result); /* Expired pieces cannot complete the newly started set. */
	EXPECT(t, !vpt1_frag_expire(env.cache, 5 * second - 1));
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, usage.sets == 1 && usage.payload == 8);
	vpt1_frag_flush_context(env.cache, &env.context);
	EXPECT(t, !send(&env, &env.scope, 2, 0, 8, true, 2, U64_MAX - 5 * second, &result));
	EXPECT(t, vpt1_frag_expire(env.cache, U64_MAX) == 1);
	EXPECT(t, !vpt1_frag_expire(env.cache, 0));
	finish(t, &env);
}

static void resource_boundaries(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct test_env env;
	unsigned int i;
	int err;

	if (!start(t, &env))
		return;
	for (i = 0; i < 16; i++)
		EXPECT(t, !send(&env, &env.scope, i, 0, 8, true, 2, 0, &result));
	EXPECT(t, send(&env, &env.scope, 16, 0, 8, true, 2, 0, &result) == -ENOSPC);
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, usage.sets == 16 && usage.payload == 128);
	vpt1_frag_flush_context(env.cache, &env.context);
	for (i = 0; i < 64; i++)
		EXPECT(t, !send(&env, &env.scope, 1, i * 8, 8, true, 2, 0, &result));
	EXPECT(t, send(&env, &env.scope, 1, 512, 8, false, 2, 0, &result) == -E2BIG);
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, !usage.sets && !usage.payload);
	/* Maximum IPv4 total, 64 pieces, all fragment packets within default M. */
	atomic_long_set(&env.budget.peak, vpt1_frag_cache_charge());
	for (i = 0; i < 64; i++) {
		err = send(&env, &env.scope, 2, i * 1024, i == 63 ? 1003 : 1024,
			   i != 63, 2, 0, &result);
		EXPECT(t, err == (i == 63 ? 1 : 0));
	}
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, usage.results == 1 && !usage.payload && !usage.sets);
	EXPECT(t, usage.charge == vpt1_frag_cache_charge() + 128U * 1024U);
	EXPECT(t, atomic_long_read(&env.budget.peak) ==
	       vpt1_frag_cache_charge() + 64U * 1024U + 128U * 1024U);
	pr_info("VPT1_FRAGMENT_OUTPUT_PEAK: result=%u peak=%ld\n",
		128U * 1024U, atomic_long_read(&env.budget.peak));
	verify(t, &env, result, 65515, 2);
	/* Isolate retained payload ceiling below the charged-memory/sets limits. */
	for (i = 0; i < 4; i++)
		EXPECT(t, !send(&env, &env.scope, i, 0, 65512, true, 2, 0, &result));
	EXPECT(t, !send(&env, &env.scope, 4, 0, 96, true, 2, 0, &result));
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, usage.payload == 262144 && usage.charge < VPT1_FRAG_MAX_CHARGE);
	EXPECT(t, send(&env, &env.scope, 5, 0, 8, true, 2, 0, &result) == -E2BIG);
	vpt1_frag_flush_context(env.cache, &env.context);
	/* Focused memory-bound fixture: eight sets, payload below 256 KiB. */
	for (i = 0; i < 7; i++)
		EXPECT(t, !send(&env, &env.scope, i, 0, 32776, true, 2, 0, &result));
	EXPECT(t, !send(&env, &env.scope, 7, 0, 16392, true, 2, 0, &result));
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, usage.charge == 524288 && usage.payload < VPT1_FRAG_MAX_PAYLOAD);
	EXPECT(t, send(&env, &env.scope, 8, 0, 8, true, 2, 0, &result) == -ENOSPC);
	pr_info("VPT1_FRAGMENT_LAYOUT: cache_charge=%zu full_charge=%zu page=%lu\n",
		vpt1_frag_cache_charge(), usage.charge, PAGE_SIZE);
	finish(t, &env);
}

static void reference_and_budget_failures(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct vpt1_frag_cache *bad;
	struct test_env env;

	if (!start(t, &env))
		return;
	bad = vpt1_frag_create(NULL, &env.budget);
	EXPECT(t, IS_ERR(bad) && PTR_ERR(bad) == -EINVAL);
	env.context.available = false;
	EXPECT(t, send(&env, &env.scope, 1, 0, 16, true, 2, 0, &result) == -ESTALE);
	env.context.available = true;
	env.budget.limit = vpt1_frag_cache_charge();
	bad = vpt1_frag_create(&ops, &env.budget);
	EXPECT(t, IS_ERR(bad) && PTR_ERR(bad) == -ENOSPC);
	EXPECT(t, send(&env, &env.scope, 1, 0, 16, true, 2, 0, &result) == -ENOSPC);
	EXPECT(t, refcount_read(&env.context.refs) == 1);
	/* The two input copies fit exactly; the simultaneous output allocation does not. The charge is
	 * the allocator bucket, which is 16 and 8 bytes on x86-64 but 64 and 64 on the Pi's arm64 kernel. */
	env.budget.limit += kmalloc_size_roundup(16) + kmalloc_size_roundup(8);
	EXPECT(t, !send(&env, &env.scope, 2, 0, 16, true, 2, 0, &result));
	EXPECT(t, send(&env, &env.scope, 2, 16, 8, false, 2, 0, &result) == -ENOSPC);
	EXPECT(t, !result);
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, !usage.sets && !usage.payload && !usage.results);
	EXPECT(t, usage.charge == vpt1_frag_cache_charge());
	finish(t, &env);
}

static void df_fragment_metadata(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_view view;
	struct test_env env;
	unsigned int df;
	u8 *data;

	if (!start(t, &env))
		return;
	for (df = 0; df < 4; df++) {
		data = fragment(1, 0, 16, true, 2);
		EXPECT(t, data);
		if (!data)
			break;
		if (df & 1)
			data[6] |= 0x40;
		fix_checksum(data);
		EXPECT(t, !vpt1_frag_submit(env.cache, &env.scope, data, 36, 0, &result));
		kfree(data);
		data = fragment(1, 16, 8, false, 2);
		EXPECT(t, data);
		if (!data)
			break;
		if (df & 2)
			data[6] |= 0x40;
		fix_checksum(data);
		EXPECT(t, vpt1_frag_submit(env.cache, &env.scope, data, 28, 0, &result) == 1);
		kfree(data);
		EXPECT(t, result);
		if (result) {
			vpt1_frag_view(result, &view);
			EXPECT(t, view.max_fragment_size == 36);
			EXPECT(t, view.frag_pmtu == !!(df & 1));
			EXPECT(t, get_unaligned_be16(view.packet + 6) == ((df & 1) ? IP_DF : 0));
			EXPECT(t, !checksum(view.packet, 20));
			vpt1_frag_result_free(result);
		}
	}
	finish(t, &env);
}

static void key_fields_and_input_ownership(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct test_env env;
	u8 before[36];
	unsigned int field;
	u8 *data, *unaligned;
	int err;

	if (!start(t, &env))
		return;
	for (field = 0; field < 4; field++) {
		EXPECT(t, !send(&env, &env.scope, 1, 0, 16, true, 2, 0, &result));
		data = fragment(1, 16, 8, false, 2);
		EXPECT(t, data);
		if (!data)
			break;
		switch (field) {
		case 0:
			data[15]++;
			break;
		case 1:
			data[19]++;
			break;
		case 2:
			data[9] = IPPROTO_TCP;
			break;
		case 3:
			data[5]++;
			break;
		}
		fix_checksum(data);
		EXPECT(t, !vpt1_frag_submit(env.cache, &env.scope, data, 28, 0, &result));
		kfree(data);
		vpt1_frag_usage(env.cache, &usage);
		EXPECT(t, usage.sets == 2 && !result);
		EXPECT(t, send(&env, &env.scope, 1, 16, 8, false, 2, 0, &result) == 1);
		verify(t, &env, result, 24, 2);
		vpt1_frag_flush_context(env.cache, &env.context);
	}
	data = fragment(2, 0, 16, true, 2);
	unaligned = kmalloc(37, GFP_KERNEL);
	EXPECT(t, data && unaligned);
	if (data && unaligned) {
		memcpy(before, data, sizeof(before));
		memcpy(unaligned + 1, data, sizeof(before));
		err = vpt1_frag_submit(env.cache, &env.scope, unaligned + 1,
				       sizeof(before), 0, &result);
		EXPECT(t, !err);
		EXPECT(t, !memcmp(before, unaligned + 1, sizeof(before)));
		/* Release input immediately; retained payload must be an independent copy. */
	}
	kfree(data);
	kfree(unaligned);
	EXPECT(t, send(&env, &env.scope, 2, 16, 8, false, 2, 0, &result) == 1);
	verify(t, &env, result, 24, 2);
	finish(t, &env);
}

static void independent_cache_instances(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct test_env first, second;
	struct vpt1_frag_usage usage;

	if (!start(t, &first))
		return;
	if (!start(t, &second)) {
		finish(t, &first);
		return;
	}
	EXPECT(t, !send(&first, &first.scope, 99, 0, 16, true, 2, 0, &result));
	EXPECT(t, !send(&second, &second.scope, 99, 16, 8, false, 2, 0, &result));
	EXPECT(t, !result);
	EXPECT(t, send(&first, &first.scope, 99, 16, 8, false, 2, 0, &result) == 1);
	verify(t, &first, result, 24, 2);
	vpt1_frag_usage(second.cache, &usage);
	EXPECT(t, usage.sets == 1 && usage.payload == 8);
	EXPECT(t, send(&second, &second.scope, 99, 0, 16, true, 2, 0, &result) == 1);
	verify(t, &second, result, 24, 2);
	finish(t, &first);
	finish(t, &second);
}

struct race_worker {
	struct test_env *env;
	struct completion ready;
	struct completion *go;
	atomic_t *errors;
	u16 id;
};

static int submit_worker(void *opaque)
{
	struct race_worker *worker = opaque;
	struct vpt1_frag_result *result;
	unsigned int i;
	int err;

	err = send(worker->env, &worker->env->scope, worker->id, 0, 16, true, 2,
		   ktime_get_boottime_ns(), &result);
	if (err)
		atomic_inc(worker->errors);
	complete(&worker->ready);
	wait_for_completion(worker->go);
	for (i = 0; i < 64; i++) {
		if (i) {
			err = send(worker->env, &worker->env->scope, worker->id + i * 4,
				   0, 16, true, 2, ktime_get_boottime_ns(), &result);
			if (err && err != -ESHUTDOWN)
				atomic_inc(worker->errors);
		}
		err = send(worker->env, &worker->env->scope, worker->id + i * 4,
			   16, 8, false, 2, ktime_get_boottime_ns(), &result);
		if (err != -ESHUTDOWN && err != 0 && err != 1)
			atomic_inc(worker->errors);
		if (result)
			vpt1_frag_result_free(result);
	}
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static void concurrent_close_and_submission(struct fragment_test *t)
{
	struct task_struct *threads[4];
	struct race_worker workers[4];
	struct completion go;
	struct test_env env;
	atomic_t errors;
	unsigned int round, i, made;

	for (round = 0; round < 8; round++) {
		if (!start(t, &env))
			return;
		init_completion(&go);
		atomic_set(&errors, 0);
		made = 0;
		for (i = 0; i < 4; i++) {
			workers[i].env = &env;
			workers[i].go = &go;
			workers[i].errors = &errors;
			workers[i].id = i;
			init_completion(&workers[i].ready);
			threads[i] = kthread_run(submit_worker, &workers[i], "vpt1-frag-test");
			EXPECT(t, !IS_ERR(threads[i]));
			if (IS_ERR(threads[i]))
				break;
			made++;
		}
		for (i = 0; i < made; i++)
			wait_for_completion(&workers[i].ready);
		complete_all(&go);
		vpt1_frag_close(env.cache);
		for (i = 0; i < made; i++)
			EXPECT(t, !kthread_stop(threads[i]));
		EXPECT(t, !atomic_read(&errors));
		finish(t, &env);
	}
}

static void reordered_clock_samples(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct test_env env;
	u64 second = NSEC_PER_SEC;
	int err;

	if (!start(t, &env))
		return;
	/* Earlier-sampled worker acquires the mutex second: this is not clock failure. */
	EXPECT(t, !send(&env, &env.scope, 1, 0, 16, true, 2, 10 * second, &result));
	err = send(&env, &env.scope, 1, 16, 8, false, 2, 10 * second - 1, &result);
	EXPECT(t, err == 1);
	verify(t, &env, result, 24, 2);
	EXPECT(t, !send(&env, &env.scope, 2, 0, 8, true, 2, 10 * second, &result));
	EXPECT(t, !send(&env, &env.scope, 2, 16, 8, false, 2, 14 * second, &result));
	EXPECT(t, !vpt1_frag_expire(env.cache, 13 * second));
	EXPECT(t, vpt1_frag_expire(env.cache, 15 * second) == 1);
	EXPECT(t, !send(&env, &env.scope, 2, 8, 8, true, 2, 14 * second, &result));
	EXPECT(t, !result);
	EXPECT(t, !vpt1_frag_expire(env.cache, 19 * second));
	EXPECT(t, vpt1_frag_expire(env.cache, 20 * second) == 1);
	vpt1_frag_usage(env.cache, &usage);
	EXPECT(t, !usage.sets && !usage.payload);
	finish(t, &env);
}

static void reentrant_context_release(struct fragment_test *t)
{
	struct vpt1_frag_result *result;
	struct vpt1_frag_usage usage;
	struct vpt1_frag_view view;
	struct test_env env;
	unsigned int path, i;
	int err;

	vpt1_frag_close(NULL);
	vpt1_frag_flush_context(NULL, NULL);
	vpt1_frag_usage(NULL, NULL);
	vpt1_frag_usage(NULL, &usage);
	EXPECT(t, usage.closed && !usage.charge && !usage.sets);
	vpt1_frag_view(NULL, NULL);
	vpt1_frag_view(NULL, &view);
	EXPECT(t, !view.packet && !view.context && !view.length);
	for (path = 0; path < 6; path++) {
		if (!start(t, &env))
			return;
		env.context.reenter = env.cache;
		if (path == 2 || path == 3)
			env.context.close_on_put = true;
		for (i = 0; i < (path == 2 || path == 3 ? 1 : 16); i++)
			EXPECT(t, !send(&env, &env.scope, i, 0, 16, true, 2, 0, &result));
		switch (path) {
		case 0:
			vpt1_frag_flush_context(env.cache, &env.context);
			break;
		case 1:
			EXPECT(t, vpt1_frag_expire(env.cache, 5ULL * NSEC_PER_SEC) == 16);
			break;
		case 2:
			err = send(&env, &env.scope, 0, 0, 16, true, 2, 0, &result);
			EXPECT(t, err == -EINVAL);
			break;
		case 3:
			EXPECT(t, send(&env, &env.scope, 0, 16, 8, false, 2, 0, &result) == 1);
			vpt1_frag_result_free(result);
			break;
		case 4:
			vpt1_frag_close(env.cache);
			break;
		case 5:
			/* Sixteen expirations plus one fresh rejection need 17 deferred puts. */
			env.budget.limit = vpt1_frag_cache_charge();
			err = send(&env, &env.scope, 99, 0, 8, true, 2,
				   5ULL * NSEC_PER_SEC, &result);
			EXPECT(t, err == -ENOSPC);
			break;
		}
		EXPECT(t, atomic_read(&env.context.puts) == (path == 2 || path == 3 ? 1 :
					      path == 5 ? 17 : 16));
		if (path == 2 || path == 3) {
			vpt1_frag_usage(env.cache, &usage);
			EXPECT(t, usage.closed);
		}
		finish(t, &env);
	}
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_fragment_cases[] = {
	KUNIT_CASE(reordering_and_output),
	KUNIT_CASE(malformed_and_poisoned_sets),
	KUNIT_CASE(overlap_range_model),
	KUNIT_CASE(ecn_merge_matrix),
	KUNIT_CASE(scope_and_invalidation),
	KUNIT_CASE(fixed_deadlines),
	KUNIT_CASE(resource_boundaries),
	KUNIT_CASE(reference_and_budget_failures),
	KUNIT_CASE(df_fragment_metadata),
	KUNIT_CASE(key_fields_and_input_ownership),
	KUNIT_CASE(independent_cache_instances),
	KUNIT_CASE(concurrent_close_and_submission),
	KUNIT_CASE(reordered_clock_samples),
	KUNIT_CASE(reentrant_context_release),
	{}
};

static struct kunit_suite vpt1_fragment_suite = {
	.name = "vpt1-fragment",
	.test_cases = vpt1_fragment_cases,
};

kunit_test_suite(vpt1_fragment_suite);
#else
static int __init vpt1_fragment_test_init(void)
{
	struct fragment_test t = {};

	reordering_and_output(&t);
	malformed_and_poisoned_sets(&t);
	overlap_range_model(&t);
	ecn_merge_matrix(&t);
	scope_and_invalidation(&t);
	fixed_deadlines(&t);
	resource_boundaries(&t);
	reference_and_budget_failures(&t);
	df_fragment_metadata(&t);
	key_fields_and_input_ownership(&t);
	independent_cache_instances(&t);
	concurrent_close_and_submission(&t);
	reordered_clock_samples(&t);
	reentrant_context_release(&t);
	pr_info("VPT1_FRAGMENT_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_fragment_test_exit(void)
{
}
module_init(vpt1_fragment_test_init);
module_exit(vpt1_fragment_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 HR3 bounded fragment normalization tests");
