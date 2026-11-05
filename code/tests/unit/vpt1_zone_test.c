// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/module.h>
#include "vpt1_zone_pool.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define zone_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct zone_test {
	unsigned int assertions;
	unsigned int failures;
};

static void zone_expect(struct zone_test *t, bool condition,
			const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_zone_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) zone_expect(t, condition, __func__, __LINE__)
#endif

static void recycle(struct zone_test *t, struct vpt1_zone_pool *pool,
		    const struct vpt1_zone_token *zone)
{
	EXPECT(t, !vpt1_zone_retire(pool, zone));
	EXPECT(t, !vpt1_zone_drained(pool, zone));
	EXPECT(t, !vpt1_zone_purged(pool, zone));
}

static void invalid_ranges_and_last_zone(struct zone_test *t)
{
	struct vpt1_zone_pool *pool;
	struct vpt1_zone_token zone = { .zone = 123 };

	EXPECT(t, !vpt1_zone_charge_bound(0, 1));
	EXPECT(t, !vpt1_zone_charge_bound(5, 4));
	pool = vpt1_zone_create(0, 1);
	EXPECT(t, IS_ERR(pool) && PTR_ERR(pool) == -EINVAL);
	pool = vpt1_zone_create(5, 4);
	EXPECT(t, IS_ERR(pool) && PTR_ERR(pool) == -EINVAL);
	EXPECT(t, vpt1_zone_reserve(NULL, &zone) == -EINVAL);
	EXPECT(t, vpt1_zone_phase(NULL, &zone) == -EINVAL);
	pool = vpt1_zone_create(65535, 65535);
	EXPECT(t, !IS_ERR(pool));
	if (IS_ERR(pool))
		return;
	EXPECT(t, vpt1_zone_phase(pool, &(struct vpt1_zone_token){ .zone = 0 }) == -EINVAL);
	EXPECT(t, vpt1_zone_phase(pool, &(struct vpt1_zone_token){ .zone = 65534 }) == -EINVAL);
	EXPECT(t, !vpt1_zone_reserve(pool, &zone) && zone.zone == 65535);
	EXPECT(t, vpt1_zone_reserve(pool, NULL) == -EINVAL);
	EXPECT(t, !vpt1_zone_confirm_clean(pool, &zone));
	recycle(t, pool, &zone);
	EXPECT(t, !vpt1_zone_destroy(pool));
}

static void lifecycle_and_quarantine(struct zone_test *t)
{
	struct vpt1_zone_pool *pool = vpt1_zone_create(1, 2);
	struct vpt1_zone_token first, second, old, sentinel = { .zone = 77 };

	EXPECT(t, !IS_ERR(pool));
	if (IS_ERR(pool))
		return;
	EXPECT(t, !vpt1_zone_reserve(pool, &first) && first.zone == 1);
	EXPECT(t, vpt1_zone_phase(pool, &first) == VPT1_ZONE_RESERVED);
	EXPECT(t, !vpt1_zone_confirm_clean(pool, &first));
	EXPECT(t, vpt1_zone_phase(pool, &first) == VPT1_ZONE_ACTIVE);
	EXPECT(t, !vpt1_zone_reserve(pool, &second) && second.zone == 2);
	EXPECT(t, vpt1_zone_destroy(pool) == -EBUSY);
	EXPECT(t, vpt1_zone_reserve(pool, &sentinel) == -ENOSPC && sentinel.zone == 77);
	EXPECT(t, !vpt1_zone_retire(pool, &first));
	EXPECT(t, vpt1_zone_purged(pool, &first) == -EBUSY);
	EXPECT(t, vpt1_zone_confirm_clean(pool, &first) == -EBUSY);
	EXPECT(t, vpt1_zone_reserve(pool, &sentinel) == -ENOSPC);
	EXPECT(t, !vpt1_zone_drained(pool, &first));
	EXPECT(t, vpt1_zone_phase(pool, &first) == VPT1_ZONE_PURGE_PENDING);
	/* Failed/as-yet-uncompleted CT purge keeps the number quarantined. */
	EXPECT(t, vpt1_zone_reserve(pool, &sentinel) == -ENOSPC);
	EXPECT(t, vpt1_zone_destroy(pool) == -EBUSY);
	old = first;
	EXPECT(t, !vpt1_zone_purged(pool, &first));
	EXPECT(t, vpt1_zone_purged(pool, &first) == -ESTALE);
	EXPECT(t, !vpt1_zone_reserve(pool, &sentinel) && sentinel.zone == first.zone);
	EXPECT(t, sentinel.serial != old.serial);
	EXPECT(t, vpt1_zone_phase(pool, &old) == -ESTALE);
	first = sentinel;
	EXPECT(t, vpt1_zone_phase(pool, &first) == VPT1_ZONE_RESERVED);
	EXPECT(t, vpt1_zone_purged(pool, &second) == -EBUSY);
	EXPECT(t, !vpt1_zone_retire(pool, &first));
	EXPECT(t, !vpt1_zone_drained(pool, &first));
	EXPECT(t, vpt1_zone_purged(pool, &old) == -ESTALE);
	EXPECT(t, vpt1_zone_phase(pool, &first) == VPT1_ZONE_PURGE_PENDING);
	EXPECT(t, !vpt1_zone_purged(pool, &first));
	recycle(t, pool, &second);
	EXPECT(t, !vpt1_zone_destroy(pool));
}

static void full_capacity_and_reuse(struct zone_test *t)
{
	struct vpt1_zone_pool *pool = vpt1_zone_create(1, 65535);
	struct vpt1_zone_token zone = {};
	unsigned int i;

	EXPECT(t, !IS_ERR(pool));
	if (IS_ERR(pool))
		return;
	for (i = 1; i <= 65535; i++) {
		EXPECT(t, !vpt1_zone_reserve(pool, &zone));
		EXPECT(t, zone.zone == i && zone.serial == i);
		EXPECT(t, vpt1_zone_phase(pool, &zone) == VPT1_ZONE_RESERVED);
	}
	zone.zone = 9;
	EXPECT(t, vpt1_zone_reserve(pool, &zone) == -ENOSPC && zone.zone == 9);
	EXPECT(t, vpt1_zone_destroy(pool) == -EBUSY);
	for (i = 1; i <= 65535; i++) {
		zone.zone = i;
		zone.serial = i;
		recycle(t, pool, &zone);
	}
	EXPECT(t, !vpt1_zone_reserve(pool, &zone) && zone.zone == 1);
	recycle(t, pool, &zone);
	EXPECT(t, !vpt1_zone_destroy(pool));
	pr_info("VPT1_ZONE_LAYOUT: full_range_charge=%zu page=%lu\n",
		vpt1_zone_charge_bound(1, 65535), PAGE_SIZE);
}

static void independent_pool_instances(struct zone_test *t)
{
	struct vpt1_zone_pool *a = vpt1_zone_create(77, 77);
	struct vpt1_zone_pool *b = vpt1_zone_create(77, 77);
	struct vpt1_zone_token za, zb;

	EXPECT(t, !IS_ERR(a) && !IS_ERR(b));
	if (IS_ERR(a) || IS_ERR(b)) {
		if (!IS_ERR(a))
			vpt1_zone_destroy(a);
		if (!IS_ERR(b))
			vpt1_zone_destroy(b);
		return;
	}
	EXPECT(t, !vpt1_zone_reserve(a, &za) && za.zone == 77);
	EXPECT(t, !vpt1_zone_reserve(b, &zb) && zb.zone == 77);
	EXPECT(t, !vpt1_zone_confirm_clean(a, &za));
	EXPECT(t, !vpt1_zone_confirm_clean(b, &zb));
	recycle(t, a, &za);
	EXPECT(t, vpt1_zone_phase(b, &zb) == VPT1_ZONE_ACTIVE);
	EXPECT(t, vpt1_zone_phase(a, &za) == -ESTALE);
	recycle(t, b, &zb);
	EXPECT(t, !vpt1_zone_destroy(a));
	EXPECT(t, !vpt1_zone_destroy(b));
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_zone_cases[] = {
	KUNIT_CASE(invalid_ranges_and_last_zone),
	KUNIT_CASE(lifecycle_and_quarantine),
	KUNIT_CASE(full_capacity_and_reuse),
	KUNIT_CASE(independent_pool_instances),
	{}
};

static struct kunit_suite vpt1_zone_suite = {
	.name = "vpt1-zone",
	.test_cases = vpt1_zone_cases,
};

kunit_test_suite(vpt1_zone_suite);
#else
static int __init vpt1_zone_test_init(void)
{
	struct zone_test t = {};

	invalid_ranges_and_last_zone(&t);
	lifecycle_and_quarantine(&t);
	full_capacity_and_reuse(&t);
	independent_pool_instances(&t);
	pr_info("VPT1_ZONE_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_zone_test_exit(void)
{
}
module_init(vpt1_zone_test_init);
module_exit(vpt1_zone_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 HR2 clean zone lifecycle and exhaustion tests");
