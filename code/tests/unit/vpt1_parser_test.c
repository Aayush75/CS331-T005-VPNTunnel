// SPDX-License-Identifier: GPL-2.0-only
/* Same cases use KUnit when configured, or a disposable-guest module otherwise. */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "vpt1_wire.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define parser_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct parser_test {
	unsigned int assertions;
	unsigned int failures;
};

static void parser_expect(struct parser_test *t, bool condition,
			  const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_parser_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) parser_expect(t, condition, __func__, __LINE__)
#endif

struct vpt1_test_vector {
	const char *name;
	const u8 *data;
	size_t len;
	u64 locator;
	u64 counter;
	u16 cap;
	u16 payload_len;
	u8 kind;
	u8 subtype;
	u8 stage;
};

#include "vpt1_parser_vectors.h"

/* Independent byte writers used only for deliberate malformed/boundary inputs. */
static void test_be16(u8 *p, unsigned int n)
{
	p[0] = n >> 8;
	p[1] = n;
}

static void test_be64(u8 *p, u64 n)
{
	int i;

	for (i = 7; i >= 0; i--) {
		p[i] = n;
		n >>= 8;
	}
}

static int parse(const struct vpt1_test_vector *v, const u8 *data, size_t len)
{
	struct vpt1_datagram datagram;
	struct vpt1_plaintext plaintext;

	if (v->stage)
		return vpt1_parse_authenticated_plaintext(data, len, v->cap, &plaintext);
	return vpt1_parse_datagram(data, len, v->cap, &datagram);
}

/* Bound the caller frame when KUnit and stack KASAN are both enabled. */
static noinline void expect_datagram_view(struct parser_test *t,
					  const struct vpt1_test_vector *v,
					  u8 *data, struct vpt1_datagram *d)
{
	EXPECT(t, !vpt1_parse_datagram(data, v->len, v->cap, d));
	EXPECT(t, d->kind == v->kind);
	EXPECT(t, d->locator == v->locator);
	EXPECT(t, d->body_len == v->len - 16);
	EXPECT(t, d->body == data + 16);
	if (v->kind == 3) {
		EXPECT(t, d->counter == v->counter);
		EXPECT(t, d->aad == data);
		EXPECT(t, d->ciphertext == data + 24);
		EXPECT(t, d->ciphertext_len == v->len - 40);
		EXPECT(t, d->tag == data + v->len - 16);
		EXPECT(t, !d->attempt && !d->noise && !d->noise_len);
	} else {
		EXPECT(t, d->attempt == data + 16);
		EXPECT(t, d->noise == data + 32);
		EXPECT(t, d->noise_len == (v->kind == 1 ? 192 : 144));
		EXPECT(t, !d->aad && !d->ciphertext && !d->tag);
	}
}

static void golden_and_alignment(struct parser_test *t)
{
	struct parser_views {
		struct vpt1_datagram d;
		struct vpt1_plaintext p;
	} *views;
	struct vpt1_datagram *d;
	struct vpt1_plaintext *p;
	unsigned int i, offset;
	u8 *storage;

	views = kzalloc(sizeof(*views), GFP_KERNEL);
	storage = kmalloc(1500, GFP_KERNEL);
	EXPECT(t, storage && views);
	if (!storage || !views) {
		kfree(storage);
		kfree(views);
		return;
	}
	d = &views->d;
	p = &views->p;
	for (i = 0; i < ARRAY_SIZE(vpt1_vectors); i++) {
		const struct vpt1_test_vector *v = &vpt1_vectors[i];

		for (offset = 0; offset < 8; offset++) {
			u8 *data = storage + offset;

			memcpy(data, v->data, v->len);
			if (v->stage) {
				EXPECT(t, !vpt1_parse_authenticated_plaintext(data, v->len,
									      v->cap, p));
				EXPECT(t, p->subtype == v->subtype);
				EXPECT(t, p->payload_len == v->payload_len);
				EXPECT(t, p->payload == data + 4);
			} else {
				expect_datagram_view(t, v, data, d);
			}
			EXPECT(t, !memcmp(data, v->data, v->len));
		}
	}
	kfree(storage);
	kfree(views);
}

static void truncation_and_trailing(struct parser_test *t)
{
	unsigned int i;
	u8 *data;
	size_t len;

	data = kmalloc(1500, GFP_KERNEL);
	EXPECT(t, data);
	if (!data)
		return;
	for (i = 0; i < ARRAY_SIZE(vpt1_vectors); i++) {
		const struct vpt1_test_vector *v = &vpt1_vectors[i];

		/* Exact-size allocations put the boundary at the allocator redzone. */
		for (len = 0; len < v->len; len++) {
			u8 *short_data = kmemdup(v->data, len, GFP_KERNEL);

			EXPECT(t, short_data);
			if (!short_data)
				break;
			EXPECT(t, parse(v, short_data, len) < 0);
			kfree(short_data);
		}
		memcpy(data, v->data, v->len);
		data[v->len] = 0;
		EXPECT(t, parse(v, data, v->len + 1) == -EMSGSIZE);
		EXPECT(t, parse(v, data, SIZE_MAX) == -EMSGSIZE);
	}
	kfree(data);
}

static void envelope_rejections(struct parser_test *t)
{
	struct vpt1_datagram out = { .locator = 42, .kind = 99 };
	unsigned int i, byte;
	u8 data[225];

	for (i = 0; i < 4; i++) {
		memcpy(data, vpt1_golden_0, 224);
		data[i] ^= 1;
		EXPECT(t, vpt1_parse_datagram(data, 224, 1428, &out) == -EPROTO);
	}
	for (byte = 1; byte <= 255; byte++) {
		memcpy(data, vpt1_golden_0, 224);
		data[5] = byte;
		EXPECT(t, vpt1_parse_datagram(data, 224, 1428, &out) == -EPROTO);
	}
	for (byte = 0; byte <= 255; byte++) {
		if (byte >= 1 && byte <= 3)
			continue;
		memcpy(data, vpt1_golden_0, 224);
		data[4] = byte;
		EXPECT(t, vpt1_parse_datagram(data, 224, 1428, &out) == -EPROTO);
	}
	memcpy(data, vpt1_golden_0, 224);
	data[15] = 1;
	EXPECT(t, vpt1_parse_datagram(data, 224, 1428, &out) == -EPROTO);
	memcpy(data, vpt1_golden_1, 176);
	memset(data + 8, 0, 8);
	EXPECT(t, vpt1_parse_datagram(data, 176, 1428, &out) == -EPROTO);
	memcpy(data, vpt1_golden_2, 60);
	memset(data + 8, 0, 8);
	EXPECT(t, vpt1_parse_datagram(data, 60, 1428, &out) == -EPROTO);
	for (i = 0; i < 2; i++) {
		const struct vpt1_test_vector *v = &vpt1_vectors[i];

		memcpy(data, v->data, v->len);
		test_be16(data + 6, v->len - 17);
		EXPECT(t, vpt1_parse_datagram(data, v->len - 1, 1428, &out) == -EMSGSIZE);
		test_be16(data + 6, v->len - 15);
		EXPECT(t, vpt1_parse_datagram(data, v->len + 1, 1428, &out) == -EMSGSIZE);
	}
	memcpy(data, vpt1_golden_0, 224);
	data[6] = 0xff;
	data[7] = 0xff;
	EXPECT(t, vpt1_parse_datagram(data, 224, 1428, &out) == -EMSGSIZE);
	EXPECT(t, out.locator == 42 && out.kind == 99);
}

static void record_boundaries_and_nonce(struct parser_test *t)
{
	static const u64 valid[] = { 0, 1, 0x010203, 1048575 };
	static const u64 invalid[] = { 1048576, 0x100000000ULL, U64_MAX };
	static const u8 expected[] = {
		0, 0, 0, 0, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct vpt1_datagram out;
	u8 data[60], nonce[13];
	unsigned int i, len;

	memcpy(data, vpt1_golden_2, sizeof(data));
	for (i = 0; i < ARRAY_SIZE(valid); i++) {
		test_be64(data + 16, valid[i]);
		EXPECT(t, !vpt1_parse_datagram(data, 60, 1428, &out));
		EXPECT(t, out.counter == valid[i]);
	}
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		test_be64(data + 16, invalid[i]);
		EXPECT(t, vpt1_parse_datagram(data, 60, 1428, &out) == -ERANGE);
	}
	memcpy(data, vpt1_golden_2, sizeof(data));
	for (len = 16; len < 44; len++) {
		test_be16(data + 6, len - 16);
		EXPECT(t, vpt1_parse_datagram(data, len, 1428, &out) == -EMSGSIZE);
	}
	/* Four encrypted prefix bytes are structurally enough; AEAD decides trust. */
	test_be16(data + 6, 28);
	EXPECT(t, !vpt1_parse_datagram(data, 44, 1428, &out));
	EXPECT(t, out.ciphertext_len == 4);
	memset(nonce, 0xff, sizeof(nonce));
	vpt1_counter_nonce(0x0102030405060708ULL, nonce + 1);
	EXPECT(t, nonce[0] == 0xff);
	EXPECT(t, !memcmp(nonce + 1, expected, 12));
	vpt1_counter_nonce(0, nonce + 1);
	EXPECT(t, !memchr_inv(nonce + 1, 0, 12));
}

static void plaintext_rejections(struct parser_test *t)
{
	struct vpt1_plaintext out = { .payload_len = 123, .subtype = 99 };
	unsigned int subtype, byte, i;
	u8 data[54];

	for (i = 5; i < ARRAY_SIZE(vpt1_vectors); i++) {
		const struct vpt1_test_vector *v = &vpt1_vectors[i];

		for (byte = 1; byte <= 255; byte++) {
			memcpy(data, v->data, v->len);
			data[1] = byte;
			EXPECT(t, vpt1_parse_authenticated_plaintext(data, v->len,
								     1428, &out) == -EPROTO);
		}
		/* Repair prefix length: tests actual fixed control or IP size rules. */
		memcpy(data, v->data, v->len);
		test_be16(data + 2, v->len - 5);
		EXPECT(t, vpt1_parse_authenticated_plaintext(data, v->len - 1,
							     1428, &out) == -EMSGSIZE);
		test_be16(data + 2, v->len - 3);
		EXPECT(t, vpt1_parse_authenticated_plaintext(data, v->len + 1,
							     1428, &out) == -EMSGSIZE);
	}
	for (subtype = 0; subtype <= 255; subtype++) {
		if (subtype >= 1 && subtype <= 10)
			continue;
		memcpy(data, vpt1_golden_6, 36);
		data[0] = subtype;
		EXPECT(t, vpt1_parse_authenticated_plaintext(data, 36, 1428, &out) == -EPROTO);
	}
	memcpy(data, vpt1_golden_5, 24);
	for (byte = 0; byte <= 255; byte++) {
		if (byte == 0x45)
			continue;
		data[4] = byte;
		EXPECT(t, vpt1_parse_authenticated_plaintext(data, 24, 1428, &out) == -EPROTO);
	}
	data[4] = 0x45;
	for (byte = 0; byte < 2; byte++) {
		test_be16(data + 6, byte ? 21 : 19);
		EXPECT(t, vpt1_parse_authenticated_plaintext(data, 24, 1428, &out) == -EMSGSIZE);
	}
	data[2] = 0xff;
	data[3] = 0xff;
	EXPECT(t, vpt1_parse_authenticated_plaintext(data, 24, 1428, &out) == -EMSGSIZE);
	EXPECT(t, out.subtype == 99 && out.payload_len == 123);
}

static void peer_cap_and_arguments(struct parser_test *t)
{
	static const u16 caps[] = { 1208, 1328, 1428 };
	static const u16 bad_caps[] = { 0, 1207, 1429, 65535 };
	struct vpt1_datagram d = {};
	struct vpt1_plaintext p = {};
	unsigned int i, j, len;
	u8 *data;

	data = kzalloc(1500, GFP_KERNEL);
	EXPECT(t, data);
	if (!data)
		return;
	for (i = 0; i < ARRAY_SIZE(caps); i++) {
		for (j = 0; j < 3; j++) {
			len = caps[i] + 43 + j;
			memcpy(data, vpt1_golden_2, 24);
			test_be16(data + 6, len - 16);
			EXPECT(t, vpt1_parse_datagram(data, len, caps[i], &d) ==
			       (j == 2 ? -EMSGSIZE : 0));
			len = caps[i] - 1 + j;
			memset(data, 0, 1500);
			data[0] = 1;
			test_be16(data + 2, len);
			data[4] = 0x45;
			test_be16(data + 6, len);
			EXPECT(t, vpt1_parse_authenticated_plaintext(data, len + 4,
								     caps[i], &p) ==
			       (j == 2 ? -EMSGSIZE : 0));
		}
	}
	for (i = 0; i < ARRAY_SIZE(bad_caps); i++) {
		EXPECT(t, vpt1_parse_datagram(data, 60, bad_caps[i], &d) == -EINVAL);
		EXPECT(t, vpt1_parse_authenticated_plaintext(data, 24,
							     bad_caps[i], &p) == -EINVAL);
	}
	EXPECT(t, vpt1_parse_datagram(NULL, 60, 1428, &d) == -EINVAL);
	EXPECT(t, vpt1_parse_datagram(data, 60, 1428, NULL) == -EINVAL);
	EXPECT(t, vpt1_parse_authenticated_plaintext(NULL, 24, 1428, &p) == -EINVAL);
	EXPECT(t, vpt1_parse_authenticated_plaintext(data, 24, 1428, NULL) == -EINVAL);
	kfree(data);
}

/* Opaque bytes cannot be authenticated or semantically inspected by framing. */
static void opaque_crypto_boundary(struct parser_test *t)
{
	struct vpt1_datagram out;
	unsigned int i;
	u8 data[224];

	for (i = 16; i < 224; i++) {
		memcpy(data, vpt1_golden_0, 224);
		data[i] ^= 0x80;
		EXPECT(t, !vpt1_parse_datagram(data, 224, 1428, &out));
	}
	for (i = 24; i < 60; i++) {
		memcpy(data, vpt1_golden_2, 60);
		data[i] ^= 0x80;
		EXPECT(t, !vpt1_parse_datagram(data, 60, 1428, &out));
	}
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_parser_cases[] = {
	KUNIT_CASE(golden_and_alignment),
	KUNIT_CASE(truncation_and_trailing),
	KUNIT_CASE(envelope_rejections),
	KUNIT_CASE(record_boundaries_and_nonce),
	KUNIT_CASE(plaintext_rejections),
	KUNIT_CASE(peer_cap_and_arguments),
	KUNIT_CASE(opaque_crypto_boundary),
	{}
};

static struct kunit_suite vpt1_parser_suite = {
	.name = "vpt1-parser",
	.test_cases = vpt1_parser_cases,
};

kunit_test_suite(vpt1_parser_suite);
#else
static int __init vpt1_test_init(void)
{
	struct parser_test t = {};

	golden_and_alignment(&t);
	truncation_and_trailing(&t);
	envelope_rejections(&t);
	record_boundaries_and_nonce(&t);
	plaintext_rejections(&t);
	peer_cap_and_arguments(&t);
	opaque_crypto_boundary(&t);
	pr_info("VPT1_PARSER_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_test_exit(void)
{
}
module_init(vpt1_test_init);
module_exit(vpt1_test_exit);
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 parser tests; KUnit or standalone disposable-guest suite");
