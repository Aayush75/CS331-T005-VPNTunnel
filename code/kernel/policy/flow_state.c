// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/time64.h>
#include <net/tcp.h>
#include "vpt1_flow_state.h"

#define HALF_OPEN_NS (15ULL * NSEC_PER_SEC)
#define TCP_IDLE_NS (120ULL * NSEC_PER_SEC)
#define CLOSING_NS (30ULL * NSEC_PER_SEC)
#define UDP_IDLE_NS (30ULL * NSEC_PER_SEC)
#define ECHO_NS (5ULL * NSEC_PER_SEC)

static bool valid_sample(const struct vpt1_tcp_sample *sample)
{
	u8 flags;

	if (!sample || sample->direction > VPT1_FLOW_REPLY)
		return false;
	flags = sample->flags;
	if ((flags & TCPHDR_FIN) && (flags & TCPHDR_RST))
		return false;
	if (flags & TCPHDR_SYN)
		return !sample->payload_len &&
		       !(flags & (TCPHDR_FIN | TCPHDR_RST | TCPHDR_PSH | TCPHDR_URG));
	if (flags & TCPHDR_RST)
		return !sample->payload_len && !(flags & (TCPHDR_PSH | TCPHDR_URG));
	return flags & TCPHDR_ACK;
}

int vpt1_tcp_begin(struct vpt1_tcp_state *state,
		   const struct vpt1_tcp_sample *sample, u64 now_ns)
{
	if (!state || !valid_sample(sample))
		return -EINVAL;
	if (state->phase != VPT1_TCP_EMPTY)
		return -EALREADY;
	if (sample->direction != VPT1_FLOW_ORIGINAL ||
	    !(sample->flags & TCPHDR_SYN) || (sample->flags & TCPHDR_ACK))
		return -EPROTO;
	state->created_ns = now_ns;
	state->last_ns = now_ns;
	state->initiator_isn = sample->sequence;
	state->phase = VPT1_TCP_SYN_PENDING;
	return 0;
}

int vpt1_tcp_check_live(struct vpt1_tcp_state *state, u64 now_ns)
{
	u64 start, limit;

	if (!state)
		return -EINVAL;
	if (state->phase == VPT1_TCP_EMPTY || state->phase == VPT1_TCP_DEAD)
		return -ENOENT;
	if (now_ns < state->last_ns)
		return -ESTALE;
	switch (state->phase) {
	case VPT1_TCP_SYN_PENDING:
	case VPT1_TCP_SYNACK_SEEN:
		start = state->created_ns;
		limit = HALF_OPEN_NS;
		break;
	case VPT1_TCP_ESTABLISHED:
		start = state->last_ns;
		limit = TCP_IDLE_NS;
		break;
	case VPT1_TCP_CLOSING:
		start = state->closing_ns;
		limit = CLOSING_NS;
		break;
	default:
		return -EINVAL;
	}
	if (now_ns - start >= limit) {
		state->phase = VPT1_TCP_DEAD;
		return -ETIME;
	}
	return 0;
}

int vpt1_tcp_advance(struct vpt1_tcp_state *state,
		     const struct vpt1_tcp_sample *sample, u64 now_ns)
{
	bool reply;
	int err;

	if (!state || !valid_sample(sample))
		return -EINVAL;
	err = vpt1_tcp_check_live(state, now_ns);
	if (err)
		return err;
	reply = sample->direction == VPT1_FLOW_REPLY;
	if (sample->flags & TCPHDR_SYN) {
		if (state->phase == VPT1_TCP_CLOSING)
			return -EPROTO;
		if (!reply) {
			if ((sample->flags & TCPHDR_ACK) ||
			    sample->sequence != state->initiator_isn)
				return -EPROTO;
		} else {
			if (!(sample->flags & TCPHDR_ACK) ||
			    sample->acknowledgment != state->initiator_isn + 1)
				return -EPROTO;
			if (state->phase == VPT1_TCP_SYN_PENDING) {
				state->responder_isn = sample->sequence;
				state->phase = VPT1_TCP_SYNACK_SEEN;
			} else if (sample->sequence != state->responder_isn) {
				return -EPROTO;
			}
		}
	} else if (state->phase == VPT1_TCP_SYN_PENDING) {
		/* A correlated refusal may return before any SYN-ACK. */
		if (!reply || (sample->flags & (TCPHDR_RST | TCPHDR_ACK)) !=
		    (TCPHDR_RST | TCPHDR_ACK) ||
		    sample->acknowledgment != state->initiator_isn + 1)
			return -EPROTO;
	} else if (state->phase == VPT1_TCP_SYNACK_SEEN) {
		if (!(sample->flags & TCPHDR_ACK))
			return -EPROTO;
		if (reply) {
			if (!(sample->flags & TCPHDR_RST) ||
			    sample->sequence != state->responder_isn + 1 ||
			    sample->acknowledgment != state->initiator_isn + 1)
				return -EPROTO;
		} else {
			if (sample->sequence != state->initiator_isn + 1 ||
			    sample->acknowledgment != state->responder_isn + 1)
				return -EPROTO;
			state->phase = VPT1_TCP_ESTABLISHED;
		}
	}
	if ((sample->flags & (TCPHDR_FIN | TCPHDR_RST)) &&
	    state->phase != VPT1_TCP_CLOSING) {
		state->closing_ns = now_ns;
		state->phase = VPT1_TCP_CLOSING;
	}
	state->last_ns = now_ns;
	return 0;
}

void vpt1_tcp_invalidate(struct vpt1_tcp_state *state)
{
	state->phase = VPT1_TCP_DEAD;
}

int vpt1_udp_begin(struct vpt1_udp_state *state, u8 direction, u64 now_ns)
{
	if (!state || direction > VPT1_FLOW_REPLY)
		return -EINVAL;
	if (direction != VPT1_FLOW_ORIGINAL)
		return -EACCES;
	if (state->active)
		return -EALREADY;
	state->last_ns = now_ns;
	state->active = true;
	return 0;
}

int vpt1_udp_check_live(struct vpt1_udp_state *state, u64 now_ns)
{
	if (!state)
		return -EINVAL;
	if (!state->active)
		return -ENOENT;
	if (now_ns < state->last_ns)
		return -ESTALE;
	if (now_ns - state->last_ns >= UDP_IDLE_NS) {
		state->active = false;
		return -ETIME;
	}
	return 0;
}

int vpt1_udp_touch(struct vpt1_udp_state *state, u8 direction, u64 now_ns)
{
	int err;

	if (direction > VPT1_FLOW_REPLY)
		return -EINVAL;
	err = vpt1_udp_check_live(state, now_ns);
	if (err)
		return err;
	state->last_ns = now_ns;
	return 0;
}

void vpt1_udp_invalidate(struct vpt1_udp_state *state)
{
	state->active = false;
}

int vpt1_echo_begin(struct vpt1_echo_state *state, u16 id, u16 sequence,
		    u8 direction, u64 now_ns)
{
	if (!state || direction > VPT1_FLOW_REPLY)
		return -EINVAL;
	if (direction != VPT1_FLOW_ORIGINAL)
		return -EACCES;
	if (state->active)
		return -EALREADY;
	state->identifier = id;
	state->sequence = sequence;
	state->created_ns = now_ns;
	state->active = true;
	return 0;
}

int vpt1_echo_check_live(struct vpt1_echo_state *state, u64 now_ns)
{
	if (!state)
		return -EINVAL;
	if (!state->active)
		return -ENOENT;
	if (now_ns < state->created_ns)
		return -ESTALE;
	if (now_ns - state->created_ns >= ECHO_NS) {
		state->active = false;
		return -ETIME;
	}
	return 0;
}

int vpt1_echo_reply(struct vpt1_echo_state *state, u16 id, u16 sequence,
		    u64 now_ns)
{
	int err = vpt1_echo_check_live(state, now_ns);

	if (err)
		return err;
	if (id != state->identifier || sequence != state->sequence)
		return -EPROTO;
	state->active = false;
	return 0;
}

void vpt1_echo_invalidate(struct vpt1_echo_state *state)
{
	state->active = false;
}
