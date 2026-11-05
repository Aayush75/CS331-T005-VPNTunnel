// SPDX-License-Identifier: GPL-2.0-only
/* Test-only provider conformance; never handles production keys or packets. */
#include <crypto/aead.h>
#include <linux/crypto.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define aead_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct aead_test {
	unsigned int assertions;
	unsigned int failures;
};

static void aead_expect(struct aead_test *t, bool condition,
			const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_aead_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) aead_expect(t, condition, __func__, __LINE__)
#endif

struct vpt1_aead_vector {
	const char *name;
	const u8 *key;
	const u8 *nonce;
	const u8 *aad;
	const u8 *plaintext;
	const u8 *protected;
	unsigned int aad_len;
	unsigned int plaintext_len;
};

#include "vpt1_aead_vectors.h"

/* Process context only; caller owns tfm/buffer until completion is drained. */
static int crypt_vector(struct crypto_aead *tfm, u8 *buffer,
			const struct vpt1_aead_vector *v, const u8 *nonce,
			bool encrypt)
{
	DECLARE_CRYPTO_WAIT(wait);
	struct aead_request *request;
	struct scatterlist sg;
	u8 iv[12];
	int err;

	request = aead_request_alloc(tfm, GFP_KERNEL);
	if (!request)
		return -ENOMEM;
	memcpy(iv, nonce, sizeof(iv));
	sg_init_one(&sg, buffer, v->aad_len + v->plaintext_len + 16);
	aead_request_set_callback(request, CRYPTO_TFM_REQ_MAY_BACKLOG |
				  CRYPTO_TFM_REQ_MAY_SLEEP, crypto_req_done, &wait);
	aead_request_set_ad(request, v->aad_len);
	aead_request_set_crypt(request, &sg, &sg,
			       v->plaintext_len + (encrypt ? 0 : 16), iv);
	err = crypto_wait_req(encrypt ? crypto_aead_encrypt(request) :
				      crypto_aead_decrypt(request), &wait);
	if (err && !encrypt)
		memzero_explicit(buffer + v->aad_len, v->plaintext_len + 16);
	aead_request_free(request);
	memzero_explicit(iv, sizeof(iv));
	return err;
}

static struct crypto_aead *allocate_provider(struct aead_test *t)
{
	struct crypto_aead *tfm;
	int err;

	tfm = crypto_alloc_aead("rfc7539(chacha20,poly1305)", 0, 0);
	EXPECT(t, !IS_ERR(tfm));
	if (IS_ERR(tfm))
		return NULL;
	EXPECT(t, crypto_aead_ivsize(tfm) == 12);
	err = crypto_aead_setauthsize(tfm, 16);
	EXPECT(t, !err);
	if (err) {
		crypto_free_aead(tfm);
		return NULL;
	}
	EXPECT(t, crypto_aead_authsize(tfm) == 16);
	pr_info("vpt1_aead_test: provider=%s iv=12 tag=16\n",
		crypto_aead_driver_name(tfm));
	return tfm;
}

static void known_answer_and_alignment(struct aead_test *t)
{
	struct crypto_aead *tfm;
	unsigned int i, offset;
	u8 *storage;

	tfm = allocate_provider(t);
	if (!tfm)
		return;
	storage = kmalloc(1600, GFP_KERNEL);
	EXPECT(t, storage);
	if (!storage)
		goto free_tfm;
	for (i = 0; i < ARRAY_SIZE(aead_vectors); i++) {
		const struct vpt1_aead_vector *v = &aead_vectors[i];

		EXPECT(t, crypto_aead_setkey(tfm, v->key, 31) == -EINVAL);
		EXPECT(t, !crypto_aead_setkey(tfm, v->key, 32));
		for (offset = 0; offset < 8; offset++) {
			u8 *buffer = storage + offset;

			memcpy(buffer, v->aad, v->aad_len);
			memcpy(buffer + v->aad_len, v->plaintext, v->plaintext_len);
			EXPECT(t, !crypt_vector(tfm, buffer, v, v->nonce, true));
			EXPECT(t, !memcmp(buffer, v->aad, v->aad_len));
			EXPECT(t, !memcmp(buffer + v->aad_len, v->protected,
					  v->plaintext_len + 16));
			/* Decrypt independent golden bytes, not just our own ciphertext. */
			memcpy(buffer, v->aad, v->aad_len);
			memcpy(buffer + v->aad_len, v->protected, v->plaintext_len + 16);
			EXPECT(t, !crypt_vector(tfm, buffer, v, v->nonce, false));
			EXPECT(t, !memcmp(buffer + v->aad_len, v->plaintext, v->plaintext_len));
		}
	}
	kfree_sensitive(storage);
free_tfm:
	crypto_free_aead(tfm);
}

static void tamper_each_byte_and_key_nonce(struct aead_test *t)
{
	struct crypto_aead *tfm;
	unsigned int i, byte, total;
	u8 nonce[12], key[32];
	u8 *buffer;

	tfm = allocate_provider(t);
	if (!tfm)
		return;
	buffer = kmalloc(1600, GFP_KERNEL);
	EXPECT(t, buffer);
	if (!buffer)
		goto free_tfm;
	for (i = 0; i < ARRAY_SIZE(aead_vectors); i++) {
		const struct vpt1_aead_vector *v = &aead_vectors[i];

		EXPECT(t, !crypto_aead_setkey(tfm, v->key, 32));
		total = v->aad_len + v->plaintext_len + 16;
		for (byte = 0; byte < total; byte++) {
			memcpy(buffer, v->aad, v->aad_len);
			memcpy(buffer + v->aad_len, v->protected, v->plaintext_len + 16);
			buffer[byte] ^= 1;
			EXPECT(t, crypt_vector(tfm, buffer, v, v->nonce, false) == -EBADMSG);
			EXPECT(t, !memchr_inv(buffer + v->aad_len, 0, v->plaintext_len + 16));
		}
		for (byte = 0; byte < sizeof(nonce); byte++) {
			memcpy(buffer, v->aad, v->aad_len);
			memcpy(buffer + v->aad_len, v->protected, v->plaintext_len + 16);
			memcpy(nonce, v->nonce, sizeof(nonce));
			nonce[byte] ^= 1;
			EXPECT(t, crypt_vector(tfm, buffer, v, nonce, false) == -EBADMSG);
		}
		memcpy(key, v->key, sizeof(key));
		key[0] ^= 1;
		EXPECT(t, !crypto_aead_setkey(tfm, key, sizeof(key)));
		memcpy(buffer, v->aad, v->aad_len);
		memcpy(buffer + v->aad_len, v->protected, v->plaintext_len + 16);
		EXPECT(t, crypt_vector(tfm, buffer, v, v->nonce, false) == -EBADMSG);
	}
	memzero_explicit(key, sizeof(key));
	memzero_explicit(nonce, sizeof(nonce));
	kfree_sensitive(buffer);
free_tfm:
	crypto_free_aead(tfm);
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_aead_cases[] = {
	KUNIT_CASE(known_answer_and_alignment),
	KUNIT_CASE(tamper_each_byte_and_key_nonce),
	{}
};

static struct kunit_suite vpt1_aead_suite = {
	.name = "vpt1-aead",
	.test_cases = vpt1_aead_cases,
};

kunit_test_suite(vpt1_aead_suite);
#else
static int __init vpt1_aead_test_init(void)
{
	struct aead_test t = {};

	known_answer_and_alignment(&t);
	tamper_each_byte_and_key_nonce(&t);
	pr_info("VPT1_AEAD_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_aead_test_exit(void)
{
}
module_init(vpt1_aead_test_init);
module_exit(vpt1_aead_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RFC 8439 and independent VPT1 AEAD provider conformance tests");
