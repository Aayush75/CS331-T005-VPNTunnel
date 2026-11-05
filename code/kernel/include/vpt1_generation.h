/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_GENERATION_H
#define VPT1_GENERATION_H

#include <linux/types.h>

#define VPT1_KEY_LEN 32U
/* Age is measured from key installation with suspend-inclusive boot time. */
#define VPT1_SOFT_AGE_NS (120ULL * NSEC_PER_SEC)
#define VPT1_HARD_AGE_NS (180ULL * NSEC_PER_SEC)
#define VPT1_SOFT_RECORDS 524288ULL
#define VPT1_SOFT_BYTES (512ULL << 20)

/* Worst of the age, record and byte quantities over both directions. */
enum vpt1_limit {
	VPT1_LIMIT_OK = 0,
	VPT1_LIMIT_SOFT, /* Start replacement; the generation still works. */
	VPT1_LIMIT_HARD, /* No further reservation or acceptance. */
};

/*
 * One directional key pair: separate immutable TX and RX AEAD keys, each with
 * its own counter space (vpt1_sequence.h). Opaque; created with one reference
 * and freed in process context after the last reference and a disable.
 * The state machine (pending/current/retiring) belongs to the peer, not here.
 */
struct vpt1_generation;

/* Module-global deferred-free machinery. Process context; init before create. */
int vpt1_generation_init(void);
void vpt1_generation_exit(void); /* Waits for every deferred free. */
void vpt1_generation_drain(void); /* Waits for deferred frees queued so far. */
long vpt1_generation_live(void); /* Allocated minus freed; leak accounting. */

/*
 * Process context (allocates and sets keys; may sleep). Copies both keys into
 * the AEAD transforms and keeps nothing else; the caller erases its own copies.
 * -EINVAL for equal keys (the same key and nonce would be used in both
 * directions), -ENOENT/-ENOMEM/other for provider failures. Returns an
 * ERR_PTR; on success the caller holds the first reference.
 */
struct vpt1_generation *vpt1_generation_create(const u8 tx_key[VPT1_KEY_LEN],
					       const u8 rx_key[VPT1_KEY_LEN],
					       u64 now_ns);

/* Any context. A reference keeps the keys and counters alive, nothing more. */
void vpt1_generation_get(struct vpt1_generation *gen);
/*
 * Any context, including softirq: the final put disables both directions and
 * queues the free, which erases the keys in process context. Call this module's
 * exit only after every generation is released; a put after that leaks.
 */
void vpt1_generation_put(struct vpt1_generation *gen);

/* Close both directions: no further reservation or acceptance. Idempotent. */
void vpt1_generation_disable(struct vpt1_generation *gen);

/*
 * TX: reserve a counter before encrypting (never refunded). Adds the age check
 * to vpt1_tx_reserve(): at or past the hard age it disables the generation and
 * returns -EKEYEXPIRED. now_ns is boot time; tests pass a synthetic clock.
 */
int vpt1_generation_tx_reserve(struct vpt1_generation *gen, u32 plaintext_len,
			       u64 now_ns, u64 *counter);
/* Already-reserved TX work only: age/disable check, not another usage charge.
 * The successful last counter/byte reservation remains usable at its exact
 * usage ceiling. Caller must still check current peer/generation eligibility.
 */
int vpt1_generation_reserved_tx_valid(struct vpt1_generation *gen, u64 now_ns);
/* RX: preliminary check, and acceptance only after AEAD success. */
int vpt1_generation_rx_precheck(struct vpt1_generation *gen, u64 counter,
				u64 now_ns);
int vpt1_generation_rx_accept(struct vpt1_generation *gen, u64 counter,
			      u32 plaintext_len, u64 now_ns);

enum vpt1_limit vpt1_generation_limit(struct vpt1_generation *gen, u64 now_ns);

/* Borrowed transforms for the record path. Valid while a reference is held. */
struct crypto_aead *vpt1_generation_tx_aead(const struct vpt1_generation *gen);
struct crypto_aead *vpt1_generation_rx_aead(const struct vpt1_generation *gen);

#ifdef VPT1_DEBUG
/*
 * Test knob, compiled out of release builds: fail the Nth allocation inside vpt1_generation_create() (the
 * generation, the transmit AEAD, the receive AEAD), 1-based. Returns the previous remaining count, so a
 * caller that sets 0 afterwards learns whether the armed failure fired (0 means it did).
 */
int vpt1_generation_dbg_fail_nth(int n);
#endif

#endif /* VPT1_GENERATION_H */
