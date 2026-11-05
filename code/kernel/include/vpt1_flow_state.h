/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_FLOW_STATE_H
#define VPT1_FLOW_STATE_H

#include <linux/types.h>

enum vpt1_flow_direction {
	VPT1_FLOW_ORIGINAL,
	VPT1_FLOW_REPLY,
};

enum vpt1_tcp_phase {
	VPT1_TCP_EMPTY,
	VPT1_TCP_SYN_PENDING,
	VPT1_TCP_SYNACK_SEEN,
	VPT1_TCP_ESTABLISHED,
	VPT1_TCP_CLOSING,
	VPT1_TCP_DEAD,
};

struct vpt1_tcp_state {
	u64 created_ns;
	u64 last_ns;
	u64 closing_ns;
	u32 initiator_isn;
	u32 responder_isn;
	u8 phase;
};

struct vpt1_tcp_sample {
	/* Host-order values decoded from a complete normalized TCP packet. */
	u32 sequence;
	u32 acknowledgment;
	u32 payload_len;
	u8 flags; /* TCPHDR_* from the validated TCP header. */
	u8 direction; /* ORIGINAL or REPLY relative to the saved initiating tuple. */
};

struct vpt1_udp_state {
	u64 last_ns;
	bool active;
};

struct vpt1_echo_state {
	u64 created_ns;
	u16 identifier; /* Host order; binding includes one outstanding sequence. */
	u16 sequence;
	bool active;
};

/*
 * Value-only transition helpers; these are NOT peer objects or final admission.
 * Caller owns an initially zeroed state, serializes it under the shared peer /
 * witness-bucket lock order, and holds the exact peer incarnation, continuity,
 * nonzero zone and original conntrack reference/tuple binding around each call.
 * Before packet transitions: normalized complete packet, trusted source/direction, current
 * ACL/lease/generation eligibility and conservative conntrack validation pass.
 * Transport sequence/window validation remains conntrack's responsibility.
 * now_ns is suspend-inclusive boot time, never sender or wall-clock time.
 * No allocation, pointer retention, locks, waits, skb consumption or mark minting.
 * Success advances only the witness state, not a grant to output later unchecked.
 * A deleted/revoked TCP witness cannot be recreated by advance or midstream data.
 * check_live is a non-refreshing GC query under the same serialization; it needs
 * no synthetic packet and makes expired state ineligible at the exact deadline.
 */
int vpt1_tcp_begin(struct vpt1_tcp_state *state,
		   const struct vpt1_tcp_sample *sample, u64 now_ns);
int vpt1_tcp_advance(struct vpt1_tcp_state *state,
		     const struct vpt1_tcp_sample *sample, u64 now_ns);
int vpt1_tcp_check_live(struct vpt1_tcp_state *state, u64 now_ns);
void vpt1_tcp_invalidate(struct vpt1_tcp_state *state);

/* Caller creates an association only from an ACL-permitted original datagram.
 * touch requires the exact original/reverse five-tuple and held CT binding.
 * Invalidation/expiry cannot be undone by touch; a new original admission is
 * needed to begin another UDP association. Replies never establish one.
 */
int vpt1_udp_begin(struct vpt1_udp_state *state, u8 direction, u64 now_ns);
int vpt1_udp_touch(struct vpt1_udp_state *state, u8 direction, u64 now_ns);
int vpt1_udp_check_live(struct vpt1_udp_state *state, u64 now_ns);
void vpt1_udp_invalidate(struct vpt1_udp_state *state);

/* Source/destination/peer/zone and current diagnostic-echo ACL are caller-bound.
 * An exact reply consumes one outstanding request, without extending its fixed
 * five-second lifetime. Unmatched IDs/sequences cannot refresh or consume it.
 */
int vpt1_echo_begin(struct vpt1_echo_state *state, u16 id, u16 sequence,
		    u8 direction, u64 now_ns);
int vpt1_echo_reply(struct vpt1_echo_state *state, u16 id, u16 sequence,
		    u64 now_ns);
int vpt1_echo_check_live(struct vpt1_echo_state *state, u64 now_ns);
void vpt1_echo_invalidate(struct vpt1_echo_state *state);

#endif /* VPT1_FLOW_STATE_H */
