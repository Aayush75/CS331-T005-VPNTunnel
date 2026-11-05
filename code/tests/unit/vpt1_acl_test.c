// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "vpt1_acl.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define acl_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct acl_test {
	unsigned int assertions;
	unsigned int failures;
};

static void acl_expect(struct acl_test *t, bool condition,
		       const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_acl_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) acl_expect(t, condition, __func__, __LINE__)
#endif

/* Literal N1/N2 addresses, independent of the implementation's prefix helpers. */
#define IPV4(a, b, c, d) cpu_to_be32(((u32)(a) << 24) | ((b) << 16) | \
				   ((c) << 8) | (d))

static struct vpt1_acl_domain test_domain(void)
{
	struct vpt1_acl_domain domain = {
		.lan = { .address = IPV4(10, 77, 1, 0), .length = 24 },
		.underlay = { .address = IPV4(172, 31, 250, 0), .length = 24 },
		.lan_address = IPV4(10, 77, 1, 1),
		.peers = {
			{ .prefixes = {{ .address = IPV4(10, 77, 2, 0), .length = 24 }},
			  .count = 1 },
			{ .prefixes = {{ .address = IPV4(10, 77, 3, 0), .length = 24 }},
			  .count = 1 },
		},
	};

	return domain;
}

static struct vpt1_acl_rule test_rule(void)
{
	struct vpt1_acl_rule rule = {
		.id = 1,
		.source = { .address = IPV4(10, 77, 1, 10), .length = 32 },
		.destination = { .address = IPV4(10, 77, 2, 10), .length = 32 },
		.port_first = 8080,
		.port_last = 8080,
		.peer_slot = 1,
		.direction = VPT1_ACL_LOCAL_TO_PEER,
		.protocol = IPPROTO_TCP,
	};

	return rule;
}

static struct vpt1_acl_flow test_flow(void)
{
	struct vpt1_acl_flow flow = {
		.source = IPV4(10, 77, 1, 10),
		.destination = IPV4(10, 77, 2, 10),
		.source_port = cpu_to_be16(45000),
		.destination_port = cpu_to_be16(8080),
		.peer_slot = 1,
		.direction = VPT1_ACL_LOCAL_TO_PEER,
		.protocol = IPPROTO_TCP,
	};

	return flow;
}

static struct vpt1_acl_snapshot *
make_acl(struct acl_test *t, const struct vpt1_acl_domain *domain,
	 const struct vpt1_acl_rule *rules, unsigned int count)
{
	struct vpt1_acl_snapshot *snapshot = vpt1_acl_create(domain, rules, count);

	EXPECT(t, !IS_ERR(snapshot));
	return IS_ERR(snapshot) ? NULL : snapshot;
}

static void invalid(struct acl_test *t, const struct vpt1_acl_domain *domain,
		    const struct vpt1_acl_rule *rules, unsigned int count, long error)
{
	struct vpt1_acl_snapshot *snapshot = vpt1_acl_create(domain, rules, count);

	EXPECT(t, IS_ERR(snapshot) && PTR_ERR(snapshot) == error);
	if (!IS_ERR(snapshot))
		vpt1_acl_destroy(snapshot);
}

static void n1_literal_matrix(struct acl_test *t)
{
	static const u8 hosts[] = { 0, 1, 10, 11, 12, 255 };
	static const u16 ports[] = { 0, 1, 80, 8079, 8080, 8081, 65535 };
	struct vpt1_acl_domain domain = test_domain();
	struct vpt1_acl_flow flow = test_flow();
	struct vpt1_acl_rule rules[3];
	struct vpt1_acl_snapshot *snapshot;
	unsigned int a, b, p;
	u32 expected;

	rules[0] = test_rule();
	rules[1] = rules[0];
	rules[1].id = 2;
	rules[1].source.address = IPV4(10, 77, 1, 11);
	rules[2] = rules[1];
	rules[2].id = 3;
	rules[2].destination.address = IPV4(10, 77, 2, 11);
	snapshot = make_acl(t, &domain, rules, 3);
	if (!snapshot)
		return;
	for (a = 0; a < ARRAY_SIZE(hosts); a++) {
		for (b = 0; b < ARRAY_SIZE(hosts); b++) {
			for (p = 0; p < ARRAY_SIZE(ports); p++) {
				flow = test_flow();
				flow.source = IPV4(10, 77, 1, hosts[a]);
				flow.destination = IPV4(10, 77, 2, hosts[b]);
				flow.destination_port = cpu_to_be16(ports[p]);
				expected = 0;
				if (ports[p] == 8080) {
					if (hosts[a] == 10 && hosts[b] == 10)
						expected = 1;
					if (hosts[a] == 11 && hosts[b] == 10)
						expected = 2;
					if (hosts[a] == 11 && hosts[b] == 11)
						expected = 3;
				}
				EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == expected);
				flow.protocol = IPPROTO_UDP;
				EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
			}
		}
	}
	/* A source service port never grants new reverse initiation. */
	flow = test_flow();
	flow.source = IPV4(10, 77, 2, 10);
	flow.destination = IPV4(10, 77, 1, 10);
	flow.source_port = cpu_to_be16(8080);
	flow.destination_port = cpu_to_be16(45000);
	flow.direction = VPT1_ACL_PEER_TO_LOCAL;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.destination_port = cpu_to_be16(8080);
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	EXPECT(t, !vpt1_acl_lookup(snapshot, NULL));
	EXPECT(t, !vpt1_acl_lookup(NULL, &flow));
	vpt1_acl_destroy(snapshot);
}

static void independent_host_model(struct acl_test *t)
{
	struct vpt1_acl_domain domain = test_domain();
	struct vpt1_acl_rule rule = test_rule();
	struct vpt1_acl_flow flow = test_flow();
	struct vpt1_acl_snapshot *snapshot;
	unsigned int a, b;
	bool expected;

	rule.source = domain.lan;
	rule.destination = domain.peers[0].prefixes[0];
	rule.id = 7;
	snapshot = make_acl(t, &domain, &rule, 1);
	if (!snapshot)
		return;
	/* Literal host-number oracle, not the implementation's masks/helpers. */
	for (a = 0; a < 256; a++) {
		for (b = 0; b < 256; b++) {
			flow.source = IPV4(10, 77, 1, a);
			flow.destination = IPV4(10, 77, 2, b);
			flow.peer_slot = 1;
			expected = a > 1 && a < 255 && b > 0 && b < 255;
			EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == (expected ? 7 : 0));
			EXPECT(t, vpt1_acl_allows_rule(snapshot, 7, &flow) == expected);
			/* The same addresses cannot be attributed to peer C's slot. */
			flow.peer_slot = 2;
			EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
		}
	}
	vpt1_acl_destroy(snapshot);
}

static void invalid_domains_and_rules(struct acl_test *t)
{
	static const struct vpt1_acl_prefix rejected[] = {
		{ .address = IPV4(169, 254, 0, 0), .length = 15 },
		{ .address = IPV4(192, 0, 0, 0), .length = 2 },
		{ .address = IPV4(128, 0, 0, 0), .length = 1 },
	};
	static const struct vpt1_acl_prefix adjacent[] = {
		{ .address = IPV4(126, 0, 0, 0), .length = 8 },
		{ .address = IPV4(128, 0, 0, 0), .length = 8 },
		{ .address = IPV4(169, 253, 0, 0), .length = 16 },
		{ .address = IPV4(169, 255, 0, 0), .length = 16 },
		{ .address = IPV4(223, 255, 255, 0), .length = 24 },
	};
	static const u32 unsupported[] = { 0, 0x7f000000, 0xa9fe0000,
					  0xe0000000, 0xf0000000 };
	struct vpt1_acl_domain domain = test_domain();
	struct vpt1_acl_snapshot *snapshot;
	struct vpt1_acl_rule rules[2];
	unsigned int i;

	rules[0] = test_rule();
	invalid(t, NULL, rules, 1, -EINVAL);
	invalid(t, &domain, NULL, 1, -EINVAL);
	invalid(t, &domain, NULL, 129, -E2BIG);
	for (i = 0; i < ARRAY_SIZE(unsupported); i++) {
		domain = test_domain();
		domain.peers[0].prefixes[0].address = cpu_to_be32(unsupported[i]);
		domain.peers[0].prefixes[0].length = 32;
		invalid(t, &domain, rules, 1, -EINVAL);
	}
	domain = test_domain();
	domain.lan.length = 0;
	invalid(t, &domain, rules, 1, -EINVAL);
	domain.lan.length = 33;
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.lan.address = IPV4(10, 77, 1, 1);
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.lan_address = IPV4(10, 77, 1, 0);
	invalid(t, &domain, rules, 1, -EINVAL);
	domain.lan_address = IPV4(10, 77, 1, 255);
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.underlay = domain.lan;
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.peers[0].count = 5;
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.peers[1] = domain.peers[0];
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.peers[0].count = 2;
	domain.peers[0].prefixes[1] = domain.peers[0].prefixes[0];
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	domain.peers[0].prefixes[0] = domain.lan;
	invalid(t, &domain, rules, 1, -EINVAL);
	domain.peers[0].prefixes[0] = domain.underlay;
	invalid(t, &domain, rules, 1, -EINVAL);
	domain = test_domain();
	/* A broad canonical prefix whose range includes blocked 127/8. */
	domain.peers[0].prefixes[0].address = IPV4(64, 0, 0, 0);
	domain.peers[0].prefixes[0].length = 2;
	invalid(t, &domain, rules, 1, -EINVAL);
	for (i = 0; i < ARRAY_SIZE(rejected); i++) {
		domain = test_domain();
		domain.peers[0].prefixes[0] = rejected[i];
		invalid(t, &domain, NULL, 0, -EINVAL);
	}
	for (i = 0; i < ARRAY_SIZE(adjacent); i++) {
		domain = test_domain();
		domain.peers[0].prefixes[0] = adjacent[i];
		snapshot = make_acl(t, &domain, NULL, 0);
		vpt1_acl_destroy(snapshot);
	}
	domain = test_domain();
	for (i = 0; i < 9; i++) {
		rules[0] = test_rule();
		switch (i) {
		case 0:
			rules[0].id = 0;
			break;
		case 1:
			rules[0].peer_slot = 0;
			break;
		case 2:
			rules[0].peer_slot = 5;
			break;
		case 3:
			rules[0].direction = 0;
			break;
		case 4:
			rules[0].protocol = IPPROTO_GRE;
			break;
		case 5:
			rules[0].port_first = 0;
			break;
		case 6:
			rules[0].port_last = 8079;
			break;
		case 7:
			rules[0].peer_slot = 2;
			break;
		case 8:
			rules[0].source.length = 24;
			break;
		}
		invalid(t, &domain, rules, 1, -EINVAL);
	}
	rules[0] = test_rule();
	rules[1] = rules[0];
	rules[1].destination.address = IPV4(10, 77, 2, 11);
	invalid(t, &domain, rules, 2, -EEXIST);
	rules[1] = rules[0];
	rules[1].id = 2;
	invalid(t, &domain, rules, 2, -EEXIST);
}

static void union_and_current_rule_identity(struct acl_test *t)
{
	struct vpt1_acl_domain domain = test_domain();
	struct vpt1_acl_flow flow = test_flow();
	struct vpt1_acl_rule rules[3], swap;
	struct vpt1_acl_snapshot *old, *replacement, *reordered;

	rules[0] = test_rule();
	rules[0].id = 20;
	rules[1] = rules[0];
	rules[1].id = 10;
	rules[1].source = domain.lan;
	rules[2] = test_rule();
	rules[2].id = 30;
	rules[2].source.address = IPV4(10, 77, 1, 11);
	rules[2].destination.address = IPV4(10, 77, 2, 11);
	old = make_acl(t, &domain, rules, 3);
	if (!old)
		return;
	EXPECT(t, vpt1_acl_lookup(old, &flow) == 10);
	EXPECT(t, vpt1_acl_allows_rule(old, 20, &flow));
	swap = rules[0];
	rules[0] = rules[1];
	rules[1] = swap;
	reordered = make_acl(t, &domain, rules, 3);
	if (!reordered)
		goto out_old;
	EXPECT(t, vpt1_acl_lookup(reordered, &flow) == 10);
	vpt1_acl_destroy(reordered);
	/* Removing the saved grant cannot transfer its witness to rule 20. */
	rules[0] = rules[1];
	rules[1] = rules[2];
	replacement = make_acl(t, &domain, rules, 2);
	if (!replacement)
		goto out_old;
	EXPECT(t, vpt1_acl_lookup(replacement, &flow) == 20);
	EXPECT(t, !vpt1_acl_allows_rule(replacement, 10, &flow));
	EXPECT(t, vpt1_acl_allows_rule(replacement, 20, &flow));
	EXPECT(t, !vpt1_acl_allows_rule(replacement, 0, &flow));
	vpt1_acl_destroy(replacement);
	/* Selective B2 removal preserves B's saved rule, then narrowing beats it. */
	replacement = make_acl(t, &domain, rules, 1);
	if (!replacement)
		goto out_old;
	EXPECT(t, vpt1_acl_allows_rule(replacement, 20, &flow));
	flow.source = IPV4(10, 77, 1, 11);
	flow.destination = IPV4(10, 77, 2, 11);
	EXPECT(t, !vpt1_acl_allows_rule(replacement, 30, &flow));
	vpt1_acl_destroy(replacement);
	rules[0].port_first = 8081;
	rules[0].port_last = 8081;
	replacement = make_acl(t, &domain, rules, 1);
	if (!replacement)
		goto out_old;
	flow = test_flow();
	EXPECT(t, !vpt1_acl_allows_rule(replacement, 20, &flow));
	EXPECT(t, vpt1_acl_allows_rule(old, 20, &flow));
	/* Failed replacement leaves the still-owned old snapshot unmodified. */
	rules[0].port_last = 1;
	invalid(t, &domain, rules, 1, -EINVAL);
	EXPECT(t, vpt1_acl_lookup(old, &flow) == 10);
	vpt1_acl_destroy(replacement);
out_old:
	vpt1_acl_destroy(old);
}

static void protocols_and_peer_direction(struct acl_test *t)
{
	struct vpt1_acl_domain domain = test_domain();
	struct vpt1_acl_rule rules[3];
	struct vpt1_acl_flow flow = test_flow();
	struct vpt1_acl_snapshot *snapshot;
	unsigned int port;

	rules[0] = test_rule();
	rules[0].protocol = IPPROTO_UDP;
	rules[0].port_first = 1000;
	rules[0].port_last = 1002;
	rules[1] = test_rule();
	rules[1].id = 2;
	rules[1].protocol = IPPROTO_ICMP;
	rules[1].port_first = 0;
	rules[1].port_last = 0;
	rules[2] = test_rule();
	rules[2].id = 3;
	rules[2].source.address = IPV4(10, 77, 3, 10);
	rules[2].destination.address = IPV4(10, 77, 1, 10);
	rules[2].peer_slot = 2;
	rules[2].direction = VPT1_ACL_PEER_TO_LOCAL;
	snapshot = make_acl(t, &domain, rules, 3);
	if (!snapshot)
		return;
	flow.protocol = IPPROTO_UDP;
	for (port = 0; port <= 65535; port++) {
		flow.destination_port = cpu_to_be16(port);
		EXPECT(t, vpt1_acl_lookup(snapshot, &flow) ==
		       (port >= 1000 && port <= 1002 ? 1 : 0));
	}
	flow.protocol = IPPROTO_ICMP;
	flow.source_port = 0;
	flow.destination_port = 0;
	flow.icmp_type = ICMP_ECHO;
	EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == 2);
	flow.icmp_code = 1;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.icmp_code = 0;
	flow.icmp_type = ICMP_ECHOREPLY;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.icmp_type = ICMP_ECHO;
	flow.destination_port = cpu_to_be16(1);
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow = test_flow();
	flow.source = IPV4(10, 77, 3, 10);
	flow.destination = IPV4(10, 77, 1, 10);
	flow.direction = VPT1_ACL_PEER_TO_LOCAL;
	flow.peer_slot = 2;
	EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == 3);
	flow.peer_slot = 1;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.peer_slot = 0;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.peer_slot = 5;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.direction = 0;
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	vpt1_acl_destroy(snapshot);
	rules[1].port_last = 1;
	invalid(t, &domain, rules, 3, -EINVAL);
}

static void capacity_copy_and_host_routes(struct acl_test *t)
{
	struct vpt1_acl_domain domain = test_domain();
	struct vpt1_acl_flow flow = test_flow();
	struct vpt1_acl_snapshot *snapshot;
	struct vpt1_acl_rule *rules;
	unsigned int i, p, n;

	rules = kcalloc(VPT1_ACL_MAX_RULES, sizeof(*rules), GFP_KERNEL);
	EXPECT(t, rules);
	if (!rules)
		return;
	for (i = 0; i < VPT1_ACL_MAX_RULES; i++) {
		rules[i] = test_rule();
		rules[i].id = i + 1;
		rules[i].port_first = i + 1;
		rules[i].port_last = i + 1;
	}
	snapshot = make_acl(t, &domain, rules, VPT1_ACL_MAX_RULES);
	if (!snapshot)
		goto out;
	EXPECT(t, vpt1_acl_charge(snapshot) <= vpt1_acl_charge_bound(128));
	pr_info("VPT1_ACL_PAGE_SIZE: %lu\n", PAGE_SIZE);
	pr_info("VPT1_ACL_LAYOUT: prefix=%zu/%zu rule=%zu/%zu domain=%zu/%zu flow=%zu/%zu max_charge=%zu accounted_charge=%zu\n",
		sizeof(struct vpt1_acl_prefix), __alignof__(struct vpt1_acl_prefix),
		sizeof(struct vpt1_acl_rule), __alignof__(struct vpt1_acl_rule),
		sizeof(domain), __alignof__(domain), sizeof(flow), __alignof__(flow),
		vpt1_acl_charge_bound(128), vpt1_acl_charge(snapshot));
	/* Mutating the unpublished input cannot mutate the copied snapshot. */
	rules[0].port_last = 65535;
	domain.peers[0].count = 0;
	for (i = 1; i <= 129; i++) {
		flow.destination_port = cpu_to_be16(i);
		EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == (i <= 128 ? i : 0));
	}
	vpt1_acl_destroy(snapshot);
	EXPECT(t, !vpt1_acl_charge_bound(129));
	domain = test_domain();
	snapshot = make_acl(t, &domain, NULL, 0);
	if (snapshot) {
		EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
		vpt1_acl_destroy(snapshot);
	}
	/* Exercise all four slots with all sixteen disjoint owned prefixes. */
	for (p = 0; p < VPT1_ACL_MAX_PEERS; p++) {
		domain.peers[p].count = VPT1_ACL_MAX_PREFIXES;
		for (n = 0; n < VPT1_ACL_MAX_PREFIXES; n++) {
			domain.peers[p].prefixes[n].address = IPV4(10, 80, p * 4 + n, 0);
			domain.peers[p].prefixes[n].length = 24;
		}
	}
	rules[0] = test_rule();
	rules[0].peer_slot = 4;
	rules[0].destination.address = IPV4(10, 80, 15, 10);
	snapshot = make_acl(t, &domain, rules, 1);
	if (snapshot) {
		flow = test_flow();
		flow.peer_slot = 4;
		flow.destination = IPV4(10, 80, 15, 10);
		EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == 1);
		vpt1_acl_destroy(snapshot);
	}
	/* /31 uses both endpoints, /32 remote ownership remains supported. */
	domain = test_domain();
	domain.lan.address = IPV4(10, 77, 1, 10);
	domain.lan.length = 31;
	domain.lan_address = IPV4(10, 77, 1, 10);
	domain.peers[0].prefixes[0].address = IPV4(10, 77, 2, 10);
	domain.peers[0].prefixes[0].length = 32;
	rules[0] = test_rule();
	rules[0].source = domain.lan;
	snapshot = make_acl(t, &domain, rules, 1);
	if (!snapshot)
		goto out;
	flow = test_flow();
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	flow.source = IPV4(10, 77, 1, 11);
	EXPECT(t, vpt1_acl_lookup(snapshot, &flow) == 1);
	flow.destination = IPV4(10, 77, 2, 11);
	EXPECT(t, !vpt1_acl_lookup(snapshot, &flow));
	vpt1_acl_destroy(snapshot);
out:
	kfree(rules);
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_acl_cases[] = {
	KUNIT_CASE(n1_literal_matrix),
	KUNIT_CASE(independent_host_model),
	KUNIT_CASE(invalid_domains_and_rules),
	KUNIT_CASE(union_and_current_rule_identity),
	KUNIT_CASE(protocols_and_peer_direction),
	KUNIT_CASE(capacity_copy_and_host_routes),
	{}
};

static struct kunit_suite vpt1_acl_suite = {
	.name = "vpt1-acl",
	.test_cases = vpt1_acl_cases,
};

kunit_test_suite(vpt1_acl_suite);
#else
static int __init vpt1_acl_test_init(void)
{
	struct acl_test t = {};

	n1_literal_matrix(&t);
	independent_host_model(&t);
	invalid_domains_and_rules(&t);
	union_and_current_rule_identity(&t);
	protocols_and_peer_direction(&t);
	capacity_copy_and_host_routes(&t);
	pr_info("VPT1_ACL_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_acl_test_exit(void)
{
}
module_init(vpt1_acl_test_init);
module_exit(vpt1_acl_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 HR2 immutable ACL and stable-rule predicate tests");
