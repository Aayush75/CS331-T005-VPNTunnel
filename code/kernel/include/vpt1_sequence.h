/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_SEQUENCE_H
#define VPT1_SEQUENCE_H

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/spinlock.h>

#define VPT1_REPLAY_BITS 1024U
#define VPT1_HARD_RECORDS 1048576ULL
#define VPT1_HARD_BYTES 1073741824ULL /* 1 GiB */
#define VPT1_MAX_PLAINTEXT 1432U

/* One instance per directional generation. Never copy or reinitialize live state. */
struct vpt1_tx_sequence {
	atomic64_t next; /* Sole writer: successful reservation under lock. */
	spinlock_t lock; /* Protects disabled and byte charge; serializes reservations. */
	u64 bytes;
	bool disabled;
};

struct vpt1_rx_sequence {
	spinlock_t lock; /* Protects every field below, including preliminary reads. */
	DECLARE_BITMAP(seen, VPT1_REPLAY_BITS);
	u64 highest;
	u64 records;
	u64 bytes;
	bool have_highest;
	bool disabled;
};

/* Initialize only before publication; caller owns memory and generation refs. */
void vpt1_tx_sequence_init(struct vpt1_tx_sequence *state);
void vpt1_rx_sequence_init(struct vpt1_rx_sequence *state);

/*
 * Non-sleeping; spin_lock_bh permits process/softirq callers (not hard IRQ/NMI).
 * Reserve exactly once before AEAD. Successful reservations are never refunded,
 * even after crypto/output failure. Output counter remains unchanged on failure.
 * Return 0, -EINVAL (length), -EDQUOT (hard usage), -EKEYREVOKED (disabled).
 * No key-age, lease, generation-lifetime or final-output authorization is implied.
 */
int vpt1_tx_reserve(struct vpt1_tx_sequence *state, u32 plaintext_len, u64 *counter);
void vpt1_tx_disable(struct vpt1_tx_sequence *state);

/* Read-only preliminary check. A successful check reserves/accepts nothing. */
int vpt1_rx_precheck(struct vpt1_rx_sequence *state, u64 counter);

/*
 * Invoke only after AEAD succeeds, before semantic parsing/delivery. Rechecks
 * replay and budgets under one lock. An authenticated malformed record consumes
 * its counter/charge; authentication failure must never call this function.
 * Also returns -ERANGE (counter bound), -ESTALE (outside window), -EALREADY
 * (duplicate). State remains unchanged on all errors. Caller must hold generation
 * refs and enforce age/eligibility and length/cap at the surrounding boundaries.
 */
int vpt1_rx_accept_authenticated(struct vpt1_rx_sequence *state, u64 counter,
				 u32 plaintext_len);
void vpt1_rx_disable(struct vpt1_rx_sequence *state);

/* Consistent snapshot of the charged usage under the state's lock. */
void vpt1_tx_usage(struct vpt1_tx_sequence *state, u64 *records, u64 *bytes);
void vpt1_rx_usage(struct vpt1_rx_sequence *state, u64 *records, u64 *bytes);

#endif /* VPT1_SEQUENCE_H */
