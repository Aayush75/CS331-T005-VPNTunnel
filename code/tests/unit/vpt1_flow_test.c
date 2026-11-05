// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/time64.h>
#include <net/tcp.h>
#include "vpt1_flow_state.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define flow_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct flow_test {
	unsigned int assertions;
	unsigned int failures;
};

static void flow_expect(struct flow_test *t, bool condition,
			const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_flow_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) flow_expect(t, condition, __func__, __LINE__)
#endif

static struct vpt1_tcp_sample syn(u32 sequence)
{
	struct vpt1_tcp_sample sample = {
		.sequence = sequence,
		.flags = TCPHDR_SYN,
		.direction = VPT1_FLOW_ORIGINAL,
	};

	return sample;
}

static void strict_handshake_and_retransmissions(struct flow_test *t)
{
	struct vpt1_tcp_state state = {};
	struct vpt1_tcp_sample sample = syn(100);

	EXPECT(t, !vpt1_tcp_begin(&state, &sample, 0));
	EXPECT(t, state.phase == VPT1_TCP_SYN_PENDING);
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 1));
	EXPECT(t, state.created_ns == 0);
	sample.sequence++;
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 2) == -EPROTO);
	EXPECT(t, state.last_ns == 1);
	sample.sequence = 900;
	sample.direction = VPT1_FLOW_REPLY;
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 2) == -EPROTO);
	sample.flags |= TCPHDR_ACK;
	sample.acknowledgment = 102;
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 2) == -EPROTO);
	sample.acknowledgment = 101;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 3));
	EXPECT(t, state.phase == VPT1_TCP_SYNACK_SEEN && state.responder_isn == 900);
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 4));
	sample.sequence++;
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 5) == -EPROTO);
	sample.sequence = 901;
	sample.acknowledgment = 101;
	sample.flags = TCPHDR_ACK;
	sample.payload_len = 1;
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 5) == -EPROTO);
	sample.direction = VPT1_FLOW_ORIGINAL;
	sample.sequence = 101;
	sample.acknowledgment = 901;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 5));
	EXPECT(t, state.phase == VPT1_TCP_ESTABLISHED);
	/* A final ACK may carry application data; later window checks belong to CT. */
	sample.payload_len = 100;
	sample.sequence = 500;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 6));
	sample = syn(900);
	sample.direction = VPT1_FLOW_REPLY;
	sample.flags |= TCPHDR_ACK;
	sample.acknowledgment = 101;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 7));
	EXPECT(t, state.phase == VPT1_TCP_ESTABLISHED);
	sample.flags = TCPHDR_ACK | TCPHDR_FIN;
	sample.sequence = 901;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 8));
	EXPECT(t, state.phase == VPT1_TCP_CLOSING && state.closing_ns == 8);
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 9));
	EXPECT(t, state.closing_ns == 8);
	sample = syn(100);
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 10) == -EPROTO);
}

static void tcp_flags_and_sequence_wrap(struct flow_test *t)
{
	struct vpt1_tcp_state state;
	struct vpt1_tcp_sample sample;
	unsigned int flags, seed;
	bool allowed;
	int err;

	for (flags = 0; flags <= 255; flags++) {
		memset(&state, 0, sizeof(state));
		sample = syn(1);
		sample.flags = flags;
		allowed = flags == 0x02 || flags == 0x42 || flags == 0x82 || flags == 0xc2;
		err = vpt1_tcp_begin(&state, &sample, 0);
		EXPECT(t, (err == 0) == allowed);
		if (!allowed)
			EXPECT(t, state.phase == VPT1_TCP_EMPTY);
	}
	for (seed = 0; seed < 1024; seed++) {
		memset(&state, 0, sizeof(state));
		sample = syn(U32_MAX - seed);
		EXPECT(t, !vpt1_tcp_begin(&state, &sample, 0));
		EXPECT(t, vpt1_tcp_begin(&state, &sample, 1) == -EALREADY);
		sample.direction = VPT1_FLOW_REPLY;
		sample.sequence = U32_MAX;
		sample.acknowledgment = 0U - seed;
		sample.flags |= TCPHDR_ACK;
		EXPECT(t, !vpt1_tcp_advance(&state, &sample, 1));
		sample.direction = VPT1_FLOW_ORIGINAL;
		sample.sequence = 0U - seed;
		sample.acknowledgment = 0;
		sample.flags = TCPHDR_ACK | TCPHDR_PSH;
		sample.payload_len = 42;
		EXPECT(t, !vpt1_tcp_advance(&state, &sample, 2));
		EXPECT(t, state.phase == VPT1_TCP_ESTABLISHED);
	}
	memset(&state, 0, sizeof(state));
	sample = syn(1);
	sample.payload_len = 1;
	EXPECT(t, vpt1_tcp_begin(&state, &sample, 0) == -EINVAL);
	sample.payload_len = 0;
	sample.direction = VPT1_FLOW_REPLY;
	EXPECT(t, vpt1_tcp_begin(&state, &sample, 0) == -EPROTO);
	sample.direction = 2;
	EXPECT(t, vpt1_tcp_begin(&state, &sample, 0) == -EINVAL);
}

static void tcp_deadlines_and_revocation(struct flow_test *t)
{
	struct vpt1_tcp_state state = {};
	struct vpt1_tcp_sample sample = syn(10);
	u64 second = NSEC_PER_SEC;

	EXPECT(t, !vpt1_tcp_begin(&state, &sample, second));
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 15 * second));
	EXPECT(t, vpt1_tcp_check_live(&state, 16 * second) == -ETIME);
	EXPECT(t, state.phase == VPT1_TCP_DEAD);
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 17 * second) == -ENOENT);
	EXPECT(t, vpt1_tcp_begin(&state, &sample, 17 * second) == -EALREADY);
	memset(&state, 0, sizeof(state));
	sample.flags = TCPHDR_ACK;
	sample.payload_len = 10;
	EXPECT(t, vpt1_tcp_begin(&state, &sample, second) == -EPROTO);
	EXPECT(t, vpt1_tcp_advance(&state, &sample, second) == -ENOENT);
	sample = syn(10);
	EXPECT(t, !vpt1_tcp_begin(&state, &sample, second));
	sample.direction = VPT1_FLOW_REPLY;
	sample.flags |= TCPHDR_ACK;
	sample.sequence = 20;
	sample.acknowledgment = 11;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 2 * second));
	sample.direction = VPT1_FLOW_ORIGINAL;
	sample.flags = TCPHDR_ACK;
	sample.sequence = 11;
	sample.acknowledgment = 21;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 3 * second));
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 2 * second) == -ESTALE);
	EXPECT(t, state.last_ns == 3 * second);
	EXPECT(t, !vpt1_tcp_check_live(&state, 123 * second - 1));
	EXPECT(t, vpt1_tcp_check_live(&state, 123 * second) == -ETIME);
	memset(&state, 0, sizeof(state));
	sample = syn(10);
	EXPECT(t, !vpt1_tcp_begin(&state, &sample, 0));
	sample.direction = VPT1_FLOW_REPLY;
	sample.flags = TCPHDR_RST | TCPHDR_ACK;
	sample.sequence = 0;
	sample.acknowledgment = 11;
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, second));
	EXPECT(t, state.phase == VPT1_TCP_CLOSING);
	EXPECT(t, !vpt1_tcp_advance(&state, &sample, 30 * second));
	EXPECT(t, vpt1_tcp_check_live(&state, 31 * second) == -ETIME);
	vpt1_tcp_invalidate(&state);
	EXPECT(t, vpt1_tcp_advance(&state, &sample, 32 * second) == -ENOENT);
}

static void udp_association_lifetime(struct flow_test *t)
{
	struct vpt1_udp_state state = {};
	u64 second = NSEC_PER_SEC;

	EXPECT(t, vpt1_udp_touch(&state, VPT1_FLOW_REPLY, 0) == -ENOENT);
	EXPECT(t, vpt1_udp_begin(&state, VPT1_FLOW_REPLY, 0) == -EACCES);
	EXPECT(t, !vpt1_udp_begin(&state, VPT1_FLOW_ORIGINAL, second));
	EXPECT(t, vpt1_udp_begin(&state, VPT1_FLOW_ORIGINAL, second) == -EALREADY);
	EXPECT(t, !vpt1_udp_touch(&state, VPT1_FLOW_REPLY, 30 * second));
	EXPECT(t, !vpt1_udp_check_live(&state, 60 * second - 1));
	EXPECT(t, vpt1_udp_touch(&state, VPT1_FLOW_REPLY, 60 * second) == -ETIME);
	EXPECT(t, vpt1_udp_touch(&state, VPT1_FLOW_ORIGINAL, 61 * second) == -ENOENT);
	EXPECT(t, !vpt1_udp_begin(&state, VPT1_FLOW_ORIGINAL, 61 * second));
	EXPECT(t, vpt1_udp_touch(&state, 2, 62 * second) == -EINVAL);
	EXPECT(t, state.last_ns == 61 * second);
	EXPECT(t, vpt1_udp_touch(&state, VPT1_FLOW_REPLY, 60 * second) == -ESTALE);
	vpt1_udp_invalidate(&state);
	EXPECT(t, vpt1_udp_touch(&state, VPT1_FLOW_REPLY, 62 * second) == -ENOENT);
}

static void echo_outstanding_requests(struct flow_test *t)
{
	struct vpt1_echo_state state = {};
	u64 second = NSEC_PER_SEC;

	EXPECT(t, vpt1_echo_reply(&state, 1, 2, second) == -ENOENT);
	EXPECT(t, vpt1_echo_begin(&state, 1, 2, VPT1_FLOW_REPLY, 0) == -EACCES);
	EXPECT(t, !vpt1_echo_begin(&state, 1, 2, VPT1_FLOW_ORIGINAL, second));
	EXPECT(t, vpt1_echo_reply(&state, 2, 2, 2 * second) == -EPROTO);
	EXPECT(t, vpt1_echo_reply(&state, 1, 3, 3 * second) == -EPROTO);
	EXPECT(t, state.created_ns == second && state.active);
	EXPECT(t, !vpt1_echo_reply(&state, 1, 2, 6 * second - 1));
	EXPECT(t, vpt1_echo_reply(&state, 1, 2, 6 * second - 1) == -ENOENT);
	EXPECT(t, !vpt1_echo_begin(&state, 0xffff, 0xffff, VPT1_FLOW_ORIGINAL, second));
	EXPECT(t, vpt1_echo_reply(&state, 0xffff, 0xffff, 6 * second) == -ETIME);
	EXPECT(t, !vpt1_echo_begin(&state, 1, 2, VPT1_FLOW_ORIGINAL, 7 * second));
	EXPECT(t, vpt1_echo_reply(&state, 1, 2, 6 * second) == -ESTALE);
	vpt1_echo_invalidate(&state);
	EXPECT(t, vpt1_echo_reply(&state, 1, 2, 8 * second) == -ENOENT);
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_flow_cases[] = {
	KUNIT_CASE(strict_handshake_and_retransmissions),
	KUNIT_CASE(tcp_flags_and_sequence_wrap),
	KUNIT_CASE(tcp_deadlines_and_revocation),
	KUNIT_CASE(udp_association_lifetime),
	KUNIT_CASE(echo_outstanding_requests),
	{}
};

static struct kunit_suite vpt1_flow_suite = {
	.name = "vpt1-flow",
	.test_cases = vpt1_flow_cases,
};

kunit_test_suite(vpt1_flow_suite);
#else
static int __init vpt1_flow_test_init(void)
{
	struct flow_test t = {};

	strict_handshake_and_retransmissions(&t);
	tcp_flags_and_sequence_wrap(&t);
	tcp_deadlines_and_revocation(&t);
	udp_association_lifetime(&t);
	echo_outstanding_requests(&t);
	pr_info("VPT1_FLOW_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	pr_info("VPT1_FLOW_LAYOUT: tcp=%zu udp=%zu echo=%zu page=%lu\n",
		sizeof(struct vpt1_tcp_state), sizeof(struct vpt1_udp_state),
		sizeof(struct vpt1_echo_state), PAGE_SIZE);
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_flow_test_exit(void)
{
}
module_init(vpt1_flow_test_init);
module_exit(vpt1_flow_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 HR2 TCP witness and UDP/echo state helper tests");
