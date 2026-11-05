// SPDX-License-Identifier: GPL-2.0-only
/* Test-only: fixed keys, synthetic clock, no production state or packets. */
#include <crypto/aead.h>
#include <linux/bottom_half.h>
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "vpt1_generation.h"
#include "vpt1_sequence.h"
#include "vpt1_wire.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define generation_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct generation_test {
	unsigned int assertions;
	unsigned int failures;
};

static void generation_expect(struct generation_test *t, bool condition,
			      const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_generation_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) generation_expect(t, condition, __func__, __LINE__)
#endif

#define T0 (1000ULL * NSEC_PER_SEC)
#define AAD_LEN VPT1_AAD_LEN
#define PT_LEN 40U

static void make_key(u8 key[VPT1_KEY_LEN], u8 seed)
{
	unsigned int i;

	for (i = 0; i < VPT1_KEY_LEN; i++)
		key[i] = seed + i;
}

/* Process context; heap buffer because the transform needs a mappable page. */
static int crypt(struct crypto_aead *tfm, u8 *buf, u64 counter, bool encrypt)
{
	DECLARE_CRYPTO_WAIT(wait);
	struct aead_request *req;
	struct scatterlist sg;
	u8 iv[VPT1_NONCE_LEN];
	int err;

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	vpt1_counter_nonce(counter, iv);
	sg_init_one(&sg, buf, AAD_LEN + PT_LEN + VPT1_TAG_LEN);
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG |
				  CRYPTO_TFM_REQ_MAY_SLEEP, crypto_req_done, &wait);
	aead_request_set_ad(req, AAD_LEN);
	aead_request_set_crypt(req, &sg, &sg, PT_LEN + (encrypt ? 0 : VPT1_TAG_LEN), iv);
	err = crypto_wait_req(encrypt ? crypto_aead_encrypt(req) :
				      crypto_aead_decrypt(req), &wait);
	aead_request_free(req);
	return err;
}

static struct vpt1_generation *make_gen(struct generation_test *t, u8 tx_seed,
					u8 rx_seed, u64 now)
{
	u8 tx[VPT1_KEY_LEN], rx[VPT1_KEY_LEN];
	struct vpt1_generation *gen;

	make_key(tx, tx_seed);
	make_key(rx, rx_seed);
	gen = vpt1_generation_create(tx, rx, now);
	EXPECT(t, !IS_ERR(gen));
	memzero_explicit(tx, sizeof(tx));
	memzero_explicit(rx, sizeof(rx));
	return IS_ERR(gen) ? NULL : gen;
}

static void release(struct vpt1_generation *gen)
{
	vpt1_generation_disable(gen);
	vpt1_generation_put(gen);
}

static void directional_keys_and_record_round_trip(struct generation_test *t)
{
	struct vpt1_generation *a, *b;
	u64 counter, first;
	u8 *buf, *expected;
	long live;
	unsigned int i;

	EXPECT(t, !vpt1_generation_init());
	live = vpt1_generation_live();
	/* Initiator TX=k1, RX=k2; responder is the mirror image. */
	a = make_gen(t, 0x10, 0x50, T0);
	b = make_gen(t, 0x50, 0x10, T0);
	buf = kzalloc(AAD_LEN + PT_LEN + VPT1_TAG_LEN, GFP_KERNEL);
	expected = kzalloc(AAD_LEN + PT_LEN, GFP_KERNEL);
	EXPECT(t, a && b && buf && expected);
	if (!a || !b || !buf || !expected)
		goto out;
	for (i = 0; i < AAD_LEN + PT_LEN; i++)
		expected[i] = (u8)(i * 7 + 1);

	EXPECT(t, vpt1_generation_live() == live + 2);
	EXPECT(t, !vpt1_generation_tx_reserve(a, PT_LEN, T0, &first));
	EXPECT(t, first == 0);
	EXPECT(t, !vpt1_generation_tx_reserve(a, PT_LEN, T0, &counter));
	EXPECT(t, counter == 1);

	memcpy(buf, expected, AAD_LEN + PT_LEN);
	EXPECT(t, !crypt(vpt1_generation_tx_aead(a), buf, counter, true));
	EXPECT(t, memcmp(buf + AAD_LEN, expected + AAD_LEN, PT_LEN));

	/* The peer's RX transform opens it; the sender's own RX transform does not. */
	{
		u8 *copy = kmemdup(buf, AAD_LEN + PT_LEN + VPT1_TAG_LEN, GFP_KERNEL);

		EXPECT(t, copy);
		if (copy) {
			EXPECT(t, crypt(vpt1_generation_rx_aead(a), copy, counter, false) == -EBADMSG);
			/* A wrong counter is a different nonce. */
			memcpy(copy, buf, AAD_LEN + PT_LEN + VPT1_TAG_LEN);
			EXPECT(t, crypt(vpt1_generation_rx_aead(b), copy, counter + 1, false) == -EBADMSG);
			kfree_sensitive(copy);
		}
		EXPECT(t, !vpt1_generation_rx_precheck(b, counter, T0));
		EXPECT(t, !crypt(vpt1_generation_rx_aead(b), buf, counter, false));
		EXPECT(t, !memcmp(buf + AAD_LEN, expected + AAD_LEN, PT_LEN));
	}
	EXPECT(t, !vpt1_generation_rx_accept(b, counter, PT_LEN, T0));
	EXPECT(t, vpt1_generation_rx_accept(b, counter, PT_LEN, T0) == -EALREADY);
	EXPECT(t, vpt1_generation_rx_precheck(b, counter, T0) == -EALREADY);

	/* The other direction uses the other transform pair and its own counters. */
	EXPECT(t, !vpt1_generation_tx_reserve(b, PT_LEN, T0, &counter));
	EXPECT(t, counter == 0);
	memcpy(buf, expected, AAD_LEN + PT_LEN);
	EXPECT(t, !crypt(vpt1_generation_tx_aead(b), buf, counter, true));
	EXPECT(t, !crypt(vpt1_generation_rx_aead(a), buf, counter, false));
	EXPECT(t, !memcmp(buf + AAD_LEN, expected + AAD_LEN, PT_LEN));

	/* One key for both directions would reuse (key, nonce): refused. */
	{
		u8 same[VPT1_KEY_LEN];
		struct vpt1_generation *bad;

		make_key(same, 0x33);
		bad = vpt1_generation_create(same, same, T0);
		EXPECT(t, IS_ERR(bad) && PTR_ERR(bad) == -EINVAL);
		if (!IS_ERR(bad))
			release(bad);
		EXPECT(t, IS_ERR(vpt1_generation_create(NULL, same, T0)));
		memzero_explicit(same, sizeof(same));
	}
	EXPECT(t, vpt1_generation_live() == live + 2);
out:
	kfree_sensitive(buf);
	kfree_sensitive(expected);
	if (a)
		release(a);
	if (b)
		release(b);
	vpt1_generation_drain();
	EXPECT(t, vpt1_generation_live() == live);
	vpt1_generation_exit();
}

static void age_limits_and_expiry(struct generation_test *t)
{
	struct vpt1_generation *gen, *rx_gen;
	u64 counter;

	EXPECT(t, !vpt1_generation_init());
	gen = make_gen(t, 0x01, 0x81, T0);
	rx_gen = make_gen(t, 0x02, 0x82, T0);
	if (!gen || !rx_gen)
		goto out;

	EXPECT(t, vpt1_generation_limit(gen, T0) == VPT1_LIMIT_OK);
	EXPECT(t, vpt1_generation_limit(gen, T0 + VPT1_SOFT_AGE_NS - 1) == VPT1_LIMIT_OK);
	EXPECT(t, vpt1_generation_limit(gen, T0 + VPT1_SOFT_AGE_NS) == VPT1_LIMIT_SOFT);
	EXPECT(t, vpt1_generation_limit(gen, T0 + VPT1_HARD_AGE_NS - 1) == VPT1_LIMIT_SOFT);
	EXPECT(t, vpt1_generation_limit(gen, T0 + VPT1_HARD_AGE_NS) == VPT1_LIMIT_HARD);

	/* Counters are never refunded: skipped outputs leave gaps. */
	EXPECT(t, !vpt1_generation_tx_reserve(gen, PT_LEN, T0, &counter) && counter == 0);
	EXPECT(t, !vpt1_generation_tx_reserve(gen, PT_LEN, T0, &counter) && counter == 1);
	/* A soft-aged generation still works. */
	EXPECT(t, !vpt1_generation_tx_reserve(gen, PT_LEN, T0 + VPT1_SOFT_AGE_NS, &counter));
	EXPECT(t, counter == 2);
	/* The hard age closes it, and it stays closed even with an earlier clock. */
	EXPECT(t, vpt1_generation_tx_reserve(gen, PT_LEN, T0 + VPT1_HARD_AGE_NS,
					     &counter) == -EKEYEXPIRED);
	EXPECT(t, vpt1_generation_tx_reserve(gen, PT_LEN, T0, &counter) == -EKEYREVOKED);
	EXPECT(t, vpt1_generation_rx_precheck(gen, 0, T0) == -EKEYREVOKED);
	EXPECT(t, vpt1_generation_rx_accept(gen, 0, PT_LEN, T0) == -EKEYREVOKED);

	/* RX at the hard age: expiry error, nothing accepted, then disabled. */
	EXPECT(t, vpt1_generation_rx_precheck(rx_gen, 0, T0 + VPT1_HARD_AGE_NS) == -EKEYEXPIRED);
	EXPECT(t, vpt1_generation_rx_accept(rx_gen, 0, PT_LEN, T0) == -EKEYREVOKED);
out:
	if (gen)
		release(gen);
	if (rx_gen)
		release(rx_gen);
	vpt1_generation_exit();
}

static void usage_limits_soft_and_hard(struct generation_test *t)
{
	struct vpt1_generation *gen, *bytes_gen;
	u64 counter, n;

	EXPECT(t, !vpt1_generation_init());
	gen = make_gen(t, 0x21, 0x61, T0);
	bytes_gen = make_gen(t, 0x22, 0x62, T0);
	if (!gen || !bytes_gen)
		goto out;

	/* Records: soft at 524,288, hard at 1,048,576 (smallest valid plaintext). */
	for (n = 0; n < VPT1_SOFT_RECORDS - 1; n++)
		if (vpt1_generation_tx_reserve(gen, 4, T0, &counter))
			break;
	EXPECT(t, n == VPT1_SOFT_RECORDS - 1);
	EXPECT(t, vpt1_generation_limit(gen, T0) == VPT1_LIMIT_OK);
	EXPECT(t, !vpt1_generation_tx_reserve(gen, 4, T0, &counter));
	EXPECT(t, vpt1_generation_limit(gen, T0) == VPT1_LIMIT_SOFT);
	for (n = VPT1_SOFT_RECORDS; n < VPT1_HARD_RECORDS; n++)
		if (vpt1_generation_tx_reserve(gen, 4, T0, &counter))
			break;
	EXPECT(t, n == VPT1_HARD_RECORDS);
	EXPECT(t, vpt1_generation_limit(gen, T0) == VPT1_LIMIT_HARD);
	EXPECT(t, !vpt1_generation_reserved_tx_valid(gen, T0));
	EXPECT(t, vpt1_generation_reserved_tx_valid(gen, T0 + VPT1_HARD_AGE_NS) ==
		  -EKEYEXPIRED);
	EXPECT(t, vpt1_generation_tx_reserve(gen, 4, T0, &counter) == -EDQUOT);

	/* Bytes: the 512 MiB soft limit comes first with maximum-size records. */
	for (n = 0; n < VPT1_SOFT_BYTES / VPT1_MAX_PLAINTEXT; n++)
		if (vpt1_generation_tx_reserve(bytes_gen, VPT1_MAX_PLAINTEXT, T0, &counter))
			break;
	EXPECT(t, n == VPT1_SOFT_BYTES / VPT1_MAX_PLAINTEXT);
	EXPECT(t, vpt1_generation_limit(bytes_gen, T0) == VPT1_LIMIT_OK);
	EXPECT(t, !vpt1_generation_tx_reserve(bytes_gen, VPT1_MAX_PLAINTEXT, T0, &counter));
	EXPECT(t, vpt1_generation_limit(bytes_gen, T0) == VPT1_LIMIT_SOFT);
out:
	if (gen)
		release(gen);
	if (bytes_gen)
		release(bytes_gen);
	vpt1_generation_drain();
	vpt1_generation_exit();
}

static void allocation_failures(struct generation_test *t)
{
#ifdef VPT1_DEBUG
	u8 tx[VPT1_KEY_LEN], rx[VPT1_KEY_LEN];
	struct vpt1_generation *gen;
	long live;
	int n, left;

	EXPECT(t, !vpt1_generation_init());
	make_key(tx, 0x31);
	make_key(rx, 0x32);
	vpt1_generation_drain();
	live = vpt1_generation_live();
	/* Fail the generation, then the transmit AEAD, then the receive AEAD: each must give -ENOMEM and free
	 * whatever was already built (the AEAD before it, the generation itself). */
	for (n = 1; n <= 3; n++) {
		vpt1_generation_dbg_fail_nth(n);
		gen = vpt1_generation_create(tx, rx, T0);
		left = vpt1_generation_dbg_fail_nth(0);
		EXPECT(t, left == 0); /* The armed failure fired. */
		EXPECT(t, IS_ERR(gen) && PTR_ERR(gen) == -ENOMEM);
		if (!IS_ERR(gen))
			release(gen);
		vpt1_generation_drain();
		EXPECT(t, vpt1_generation_live() == live);
	}
	/* A fourth would-be failure never fires: creation has only three allocation points. */
	vpt1_generation_dbg_fail_nth(4);
	gen = vpt1_generation_create(tx, rx, T0);
	left = vpt1_generation_dbg_fail_nth(0);
	EXPECT(t, left == 1 && !IS_ERR(gen));
	if (!IS_ERR(gen))
		release(gen);
	vpt1_generation_drain();
	EXPECT(t, vpt1_generation_live() == live);
	memzero_explicit(tx, sizeof(tx));
	memzero_explicit(rx, sizeof(rx));
	vpt1_generation_exit();
#endif
}

static void references_and_deferred_free(struct generation_test *t)
{
	struct vpt1_generation *gen;
	u64 counter;
	long live;

	EXPECT(t, !vpt1_generation_init());
	EXPECT(t, vpt1_generation_init() == -EBUSY);
	live = vpt1_generation_live();
	gen = make_gen(t, 0x41, 0x71, T0);
	if (!gen)
		goto out;
	EXPECT(t, vpt1_generation_live() == live + 1);

	/* A held reference keeps it alive across the owner's put. */
	vpt1_generation_get(gen);
	vpt1_generation_disable(gen);
	vpt1_generation_put(gen);
	vpt1_generation_drain();
	EXPECT(t, vpt1_generation_live() == live + 1);
	EXPECT(t, vpt1_generation_tx_reserve(gen, PT_LEN, T0, &counter) == -EKEYREVOKED);
	EXPECT(t, vpt1_generation_reserved_tx_valid(gen, T0) == -EKEYREVOKED);

	/* The last put may come from atomic context: it only queues the free. */
	local_bh_disable();
	vpt1_generation_put(gen);
	local_bh_enable();
	vpt1_generation_drain();
	EXPECT(t, vpt1_generation_live() == live);
out:
	vpt1_generation_exit();
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_generation_cases[] = {
	KUNIT_CASE(directional_keys_and_record_round_trip),
	KUNIT_CASE(age_limits_and_expiry),
	KUNIT_CASE(usage_limits_soft_and_hard),
	KUNIT_CASE(references_and_deferred_free),
	KUNIT_CASE(allocation_failures),
	{}
};

static struct kunit_suite vpt1_generation_suite = {
	.name = "vpt1-generation",
	.test_cases = vpt1_generation_cases,
};

kunit_test_suite(vpt1_generation_suite);
#else
static int __init vpt1_generation_test_init(void)
{
	struct generation_test t = {};

	directional_keys_and_record_round_trip(&t);
	age_limits_and_expiry(&t);
	usage_limits_soft_and_hard(&t);
	references_and_deferred_free(&t);
	allocation_failures(&t);
	pr_info("VPT1_GENERATION_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_generation_test_exit(void)
{
}
module_init(vpt1_generation_test_init);
module_exit(vpt1_generation_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 directional key generation lifetime, limits and record round-trip tests");
