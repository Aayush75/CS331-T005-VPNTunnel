// SPDX-License-Identifier: GPL-2.0-only
#include <crypto/aead.h>
#include <crypto/algapi.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "vpt1_generation.h"
#include "vpt1_sequence.h"
#include "vpt1_wire.h"

#define VPT1_AEAD_NAME "rfc7539(chacha20,poly1305)"

struct vpt1_generation {
	refcount_t refs;
	struct crypto_aead *tx_aead; /* Immutable after create. */
	struct crypto_aead *rx_aead; /* Immutable after create. */
	struct vpt1_tx_sequence tx;
	struct vpt1_rx_sequence rx;
	u64 installed_ns; /* Immutable; boot time at install. */
	struct work_struct free_work;
};

static struct workqueue_struct *vpt1_gen_wq;
static atomic_long_t vpt1_gen_live = ATOMIC_LONG_INIT(0);

int vpt1_generation_init(void)
{
	if (vpt1_gen_wq)
		return -EBUSY;
	/* Ordered: one free at a time, in queue order. */
	vpt1_gen_wq = alloc_ordered_workqueue("vpt1_gen", 0);
	return vpt1_gen_wq ? 0 : -ENOMEM;
}

void vpt1_generation_drain(void)
{
	if (vpt1_gen_wq)
		flush_workqueue(vpt1_gen_wq);
}

void vpt1_generation_exit(void)
{
	/* destroy_workqueue() drains queued work first. */
	if (vpt1_gen_wq)
		destroy_workqueue(vpt1_gen_wq);
	vpt1_gen_wq = NULL;
}

long vpt1_generation_live(void)
{
	return atomic_long_read(&vpt1_gen_live);
}

static struct crypto_aead *vpt1_alloc_aead(const u8 key[VPT1_KEY_LEN])
{
	struct crypto_aead *tfm;
	int err;

	/* Synchronous implementations only: the data path calls them from softirq and
	 * never waits for a completion (mask CRYPTO_ALG_ASYNC selects sync providers).
	 */
	tfm = crypto_alloc_aead(VPT1_AEAD_NAME, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm))
		return tfm;
	if (crypto_aead_ivsize(tfm) != VPT1_NONCE_LEN ||
	    crypto_aead_maxauthsize(tfm) != VPT1_TAG_LEN) {
		err = -EINVAL;
		goto free;
	}
	err = crypto_aead_setkey(tfm, key, VPT1_KEY_LEN);
	if (err)
		goto free;
	err = crypto_aead_setauthsize(tfm, VPT1_TAG_LEN);
	if (err)
		goto free;
	return tfm;
free:
	crypto_free_aead(tfm);
	return ERR_PTR(err);
}

#ifdef VPT1_DEBUG
/* Debug builds: fail the Nth allocation inside vpt1_generation_create() (1-based, spent when it fires). */
static atomic_t vpt1_gen_dbg_fail_nth = ATOMIC_INIT(0);

int vpt1_generation_dbg_fail_nth(int n)
{
	return atomic_xchg(&vpt1_gen_dbg_fail_nth, n);
}

static bool gen_alloc_fails(void)
{
	return atomic_read(&vpt1_gen_dbg_fail_nth) && atomic_dec_return(&vpt1_gen_dbg_fail_nth) == 0;
}
#else
static inline bool gen_alloc_fails(void)
{
	return false;
}
#endif

struct vpt1_generation *vpt1_generation_create(const u8 tx_key[VPT1_KEY_LEN],
					       const u8 rx_key[VPT1_KEY_LEN],
					       u64 now_ns)
{
	struct vpt1_generation *gen;
	int err;

	if (!tx_key || !rx_key || !vpt1_gen_wq)
		return ERR_PTR(-EINVAL);
	/* One key in both directions would reuse (key, nonce) at counter zero. */
	if (!crypto_memneq(tx_key, rx_key, VPT1_KEY_LEN))
		return ERR_PTR(-EINVAL);
	if (gen_alloc_fails())
		return ERR_PTR(-ENOMEM);
	gen = kzalloc(sizeof(*gen), GFP_KERNEL);
	if (!gen)
		return ERR_PTR(-ENOMEM);
	gen->tx_aead = gen_alloc_fails() ? ERR_PTR(-ENOMEM) : vpt1_alloc_aead(tx_key);
	if (IS_ERR(gen->tx_aead)) {
		err = PTR_ERR(gen->tx_aead);
		goto free_gen;
	}
	gen->rx_aead = gen_alloc_fails() ? ERR_PTR(-ENOMEM) : vpt1_alloc_aead(rx_key);
	if (IS_ERR(gen->rx_aead)) {
		err = PTR_ERR(gen->rx_aead);
		goto free_tx;
	}
	refcount_set(&gen->refs, 1);
	vpt1_tx_sequence_init(&gen->tx);
	vpt1_rx_sequence_init(&gen->rx);
	gen->installed_ns = now_ns;
	atomic_long_inc(&vpt1_gen_live);
	return gen;
free_tx:
	crypto_free_aead(gen->tx_aead);
free_gen:
	kfree_sensitive(gen);
	return ERR_PTR(err);
}

void vpt1_generation_get(struct vpt1_generation *gen)
{
	refcount_inc(&gen->refs);
}

static void vpt1_generation_free(struct work_struct *work)
{
	struct vpt1_generation *gen =
		container_of(work, struct vpt1_generation, free_work);

	crypto_free_aead(gen->tx_aead);
	crypto_free_aead(gen->rx_aead);
	kfree_sensitive(gen);
	atomic_long_dec(&vpt1_gen_live);
}

void vpt1_generation_put(struct vpt1_generation *gen)
{
	if (!refcount_dec_and_test(&gen->refs))
		return;
	/* Misuse after exit: leak the keys' memory rather than free in softirq. */
	if (WARN_ON_ONCE(!vpt1_gen_wq))
		return;
	/*
	 * No reference remains, so nothing can race the disable below; it only
	 * makes a leaked-enabled generation visible instead of silently usable.
	 */
	vpt1_tx_disable(&gen->tx);
	vpt1_rx_disable(&gen->rx);
	/* Freeing a transform can sleep; the last put may be in softirq. */
	INIT_WORK(&gen->free_work, vpt1_generation_free);
	queue_work(vpt1_gen_wq, &gen->free_work);
}

void vpt1_generation_disable(struct vpt1_generation *gen)
{
	vpt1_tx_disable(&gen->tx);
	vpt1_rx_disable(&gen->rx);
}

static bool vpt1_generation_hard_aged(const struct vpt1_generation *gen,
				      u64 now_ns)
{
	return now_ns - gen->installed_ns >= VPT1_HARD_AGE_NS;
}

int vpt1_generation_tx_reserve(struct vpt1_generation *gen, u32 plaintext_len,
			       u64 now_ns, u64 *counter)
{
	if (vpt1_generation_hard_aged(gen, now_ns)) {
		vpt1_generation_disable(gen);
		return -EKEYEXPIRED;
	}
	return vpt1_tx_reserve(&gen->tx, plaintext_len, counter);
}

int vpt1_generation_reserved_tx_valid(struct vpt1_generation *gen, u64 now_ns)
{
	bool disabled;

	if (vpt1_generation_hard_aged(gen, now_ns))
		return -EKEYEXPIRED;
	spin_lock_bh(&gen->tx.lock);
	disabled = gen->tx.disabled;
	spin_unlock_bh(&gen->tx.lock);
	return disabled ? -EKEYREVOKED : 0;
}

int vpt1_generation_rx_precheck(struct vpt1_generation *gen, u64 counter,
				u64 now_ns)
{
	if (vpt1_generation_hard_aged(gen, now_ns)) {
		vpt1_generation_disable(gen);
		return -EKEYEXPIRED;
	}
	return vpt1_rx_precheck(&gen->rx, counter);
}

int vpt1_generation_rx_accept(struct vpt1_generation *gen, u64 counter,
			      u32 plaintext_len, u64 now_ns)
{
	if (vpt1_generation_hard_aged(gen, now_ns)) {
		vpt1_generation_disable(gen);
		return -EKEYEXPIRED;
	}
	return vpt1_rx_accept_authenticated(&gen->rx, counter, plaintext_len);
}

enum vpt1_limit vpt1_generation_limit(struct vpt1_generation *gen, u64 now_ns)
{
	u64 age = now_ns - gen->installed_ns;
	u64 records, bytes;
	enum vpt1_limit worst = VPT1_LIMIT_OK;
	int i;

	if (age >= VPT1_HARD_AGE_NS)
		return VPT1_LIMIT_HARD;
	if (age >= VPT1_SOFT_AGE_NS)
		worst = VPT1_LIMIT_SOFT;
	for (i = 0; i < 2; i++) {
		if (i == 0)
			vpt1_tx_usage(&gen->tx, &records, &bytes);
		else
			vpt1_rx_usage(&gen->rx, &records, &bytes);
		if (records >= VPT1_HARD_RECORDS || bytes >= VPT1_HARD_BYTES)
			return VPT1_LIMIT_HARD;
		if (records >= VPT1_SOFT_RECORDS || bytes >= VPT1_SOFT_BYTES)
			worst = VPT1_LIMIT_SOFT;
	}
	return worst;
}

struct crypto_aead *vpt1_generation_tx_aead(const struct vpt1_generation *gen)
{
	return gen->tx_aead;
}

struct crypto_aead *vpt1_generation_rx_aead(const struct vpt1_generation *gen)
{
	return gen->rx_aead;
}
