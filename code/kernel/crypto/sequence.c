// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/lockdep.h>
#include <linux/string.h>

#include "vpt1_sequence.h"

void vpt1_tx_sequence_init(struct vpt1_tx_sequence *state)
{
	atomic64_set(&state->next, 0);
	spin_lock_init(&state->lock);
	state->bytes = 0;
	state->disabled = false;
}

void vpt1_rx_sequence_init(struct vpt1_rx_sequence *state)
{
	spin_lock_init(&state->lock);
	bitmap_zero(state->seen, VPT1_REPLAY_BITS);
	state->highest = 0;
	state->records = 0;
	state->bytes = 0;
	state->have_highest = false;
	state->disabled = false;
}

static bool vpt1_valid_plaintext_len(u32 len)
{
	return len >= 4 && len <= VPT1_MAX_PLAINTEXT;
}

int vpt1_tx_reserve(struct vpt1_tx_sequence *state, u32 plaintext_len, u64 *counter)
{
	u64 next;
	int err = 0;

	if (!counter || !vpt1_valid_plaintext_len(plaintext_len))
		return -EINVAL;
	spin_lock_bh(&state->lock);
	next = atomic64_read(&state->next);
	if (state->disabled) {
		err = -EKEYREVOKED;
	} else if (next >= VPT1_HARD_RECORDS ||
		   plaintext_len > VPT1_HARD_BYTES - state->bytes) {
		err = -EDQUOT;
	} else {
		*counter = atomic64_fetch_inc(&state->next);
		state->bytes += plaintext_len;
	}
	spin_unlock_bh(&state->lock);
	return err;
}

void vpt1_tx_disable(struct vpt1_tx_sequence *state)
{
	spin_lock_bh(&state->lock);
	state->disabled = true;
	spin_unlock_bh(&state->lock);
}

static int vpt1_rx_check_locked(struct vpt1_rx_sequence *state, u64 counter)
{
	u64 distance;

	lockdep_assert_held(&state->lock);
	if (state->disabled)
		return -EKEYREVOKED;
	if (counter >= VPT1_HARD_RECORDS)
		return -ERANGE;
	if (!state->have_highest || counter > state->highest)
		return 0;
	distance = state->highest - counter;
	if (distance >= VPT1_REPLAY_BITS)
		return -ESTALE;
	if (test_bit(distance, state->seen))
		return -EALREADY;
	return 0;
}

int vpt1_rx_precheck(struct vpt1_rx_sequence *state, u64 counter)
{
	int err;

	spin_lock_bh(&state->lock);
	err = vpt1_rx_check_locked(state, counter);
	spin_unlock_bh(&state->lock);
	return err;
}

int vpt1_rx_accept_authenticated(struct vpt1_rx_sequence *state, u64 counter,
				 u32 plaintext_len)
{
	u64 distance;
	int err;

	if (!vpt1_valid_plaintext_len(plaintext_len))
		return -EINVAL;
	spin_lock_bh(&state->lock);
	err = vpt1_rx_check_locked(state, counter);
	if (err)
		goto out;
	if (state->records >= VPT1_HARD_RECORDS ||
	    plaintext_len > VPT1_HARD_BYTES - state->bytes) {
		err = -EDQUOT;
		goto out;
	}
	if (!state->have_highest) {
		state->highest = counter;
		state->have_highest = true;
		__set_bit(0, state->seen);
	} else if (counter > state->highest) {
		distance = counter - state->highest;
		if (distance >= VPT1_REPLAY_BITS)
			bitmap_zero(state->seen, VPT1_REPLAY_BITS);
		else
			bitmap_shift_left(state->seen, state->seen, distance,
					  VPT1_REPLAY_BITS);
		state->highest = counter;
		__set_bit(0, state->seen);
	} else {
		__set_bit(state->highest - counter, state->seen);
	}
	state->records++;
	state->bytes += plaintext_len;
out:
	spin_unlock_bh(&state->lock);
	return err;
}

void vpt1_rx_disable(struct vpt1_rx_sequence *state)
{
	spin_lock_bh(&state->lock);
	state->disabled = true;
	spin_unlock_bh(&state->lock);
}

void vpt1_tx_usage(struct vpt1_tx_sequence *state, u64 *records, u64 *bytes)
{
	spin_lock_bh(&state->lock);
	*records = atomic64_read(&state->next);
	*bytes = state->bytes;
	spin_unlock_bh(&state->lock);
}

void vpt1_rx_usage(struct vpt1_rx_sequence *state, u64 *records, u64 *bytes)
{
	spin_lock_bh(&state->lock);
	*records = state->records;
	*bytes = state->bytes;
	spin_unlock_bh(&state->lock);
}
