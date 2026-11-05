// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include "vpt1_acl.h"

/* Immutable after create; lifetime/publication belongs to the shared runtime. */
struct vpt1_acl_snapshot {
	struct vpt1_acl_domain domain;
	unsigned int count;
	struct vpt1_acl_rule rules[] __counted_by(count);
};

/* Only call after validating a prefix length in 1..32. */
static u32 prefix_mask(u8 length)
{
	return U32_MAX << (32 - length);
}

static bool prefix_contains(const struct vpt1_acl_prefix *outer,
			    const struct vpt1_acl_prefix *inner)
{
	return inner->length >= outer->length &&
	       !((be32_to_cpu(outer->address) ^ be32_to_cpu(inner->address)) &
		 prefix_mask(outer->length));
}

static bool prefix_overlaps(const struct vpt1_acl_prefix *a,
			    const struct vpt1_acl_prefix *b)
{
	return prefix_contains(a, b) || prefix_contains(b, a);
}

static bool prefix_valid(const struct vpt1_acl_prefix *prefix)
{
	u32 first, last, mask;

	if (!prefix->length || prefix->length > 32)
		return false;
	mask = prefix_mask(prefix->length);
	first = be32_to_cpu(prefix->address);
	if (first & ~mask)
		return false;
	last = first | ~mask;
	/* Reject any overlap with unsupported unicast ranges, not just the base. */
	return first > 0x00ffffff && last < 0xe0000000 &&
	       !(first <= 0x7fffffff && last >= 0x7f000000) &&
	       !(first <= 0xa9feffff && last >= 0xa9fe0000);
}

static bool host_in_prefix(__be32 address, const struct vpt1_acl_prefix *prefix)
{
	struct vpt1_acl_prefix host = { .address = address, .length = 32 };
	u32 value = be32_to_cpu(address);
	u32 base = be32_to_cpu(prefix->address);

	if (!prefix_contains(prefix, &host))
		return false;
	/* /31 has two unicast hosts; a remote /32 is a host route. */
	return prefix->length >= 31 ||
	       (value != base && value != (base | ~prefix_mask(prefix->length)));
}

static bool domain_valid(const struct vpt1_acl_domain *domain)
{
	unsigned int p, n, q, m;

	if (!domain || !prefix_valid(&domain->lan) ||
	    domain->lan.length == 32 || !prefix_valid(&domain->underlay) ||
	    prefix_overlaps(&domain->lan, &domain->underlay) ||
	    !host_in_prefix(domain->lan_address, &domain->lan))
		return false;
	for (p = 0; p < VPT1_ACL_MAX_PEERS; p++) {
		if (domain->peers[p].count > VPT1_ACL_MAX_PREFIXES)
			return false;
		for (n = 0; n < domain->peers[p].count; n++) {
			const struct vpt1_acl_prefix *prefix;

			prefix = &domain->peers[p].prefixes[n];
			if (!prefix_valid(prefix) ||
			    prefix_overlaps(prefix, &domain->lan) ||
			    prefix_overlaps(prefix, &domain->underlay))
				return false;
			/* Earlier peers/prefixes have already been validated. */
			for (q = 0; q <= p; q++)
				for (m = 0; m < (q == p ? n : domain->peers[q].count); m++)
					if (prefix_overlaps(prefix, &domain->peers[q].prefixes[m]))
						return false;
		}
	}
	return true;
}

static bool peer_contains(const struct vpt1_acl_owner *peer,
			  const struct vpt1_acl_prefix *prefix)
{
	unsigned int i;

	for (i = 0; i < peer->count; i++)
		if (prefix_contains(&peer->prefixes[i], prefix))
			return true;
	return false;
}

static bool rule_valid(const struct vpt1_acl_domain *domain,
		       const struct vpt1_acl_rule *rule)
{
	const struct vpt1_acl_owner *peer;

	if (!rule->id || !rule->peer_slot || rule->peer_slot > VPT1_ACL_MAX_PEERS ||
	    !prefix_valid(&rule->source) || !prefix_valid(&rule->destination))
		return false;
	peer = &domain->peers[rule->peer_slot - 1];
	if (rule->direction == VPT1_ACL_LOCAL_TO_PEER) {
		if (!prefix_contains(&domain->lan, &rule->source) ||
		    !peer_contains(peer, &rule->destination))
			return false;
	} else if (rule->direction == VPT1_ACL_PEER_TO_LOCAL) {
		if (!peer_contains(peer, &rule->source) ||
		    !prefix_contains(&domain->lan, &rule->destination))
			return false;
	} else {
		return false;
	}
	switch (rule->protocol) {
	case IPPROTO_TCP:
	case IPPROTO_UDP:
		return rule->port_first && rule->port_first <= rule->port_last;
	case IPPROTO_ICMP:
		return !rule->port_first && !rule->port_last;
	default:
		return false;
	}
}

static bool same_match(const struct vpt1_acl_rule *a,
		       const struct vpt1_acl_rule *b)
{
	return a->peer_slot == b->peer_slot && a->direction == b->direction &&
	       a->protocol == b->protocol && a->port_first == b->port_first &&
	       a->port_last == b->port_last && a->source.address == b->source.address &&
	       a->source.length == b->source.length &&
	       a->destination.address == b->destination.address &&
	       a->destination.length == b->destination.length;
}

size_t vpt1_acl_charge_bound(unsigned int count)
{
	struct vpt1_acl_snapshot *snapshot;

	if (count > VPT1_ACL_MAX_RULES)
		return 0;
	return kmalloc_size_roundup(struct_size(snapshot, rules, count));
}

#ifdef VPT1_DEBUG
/* Debug builds: fail the Nth snapshot allocation (1-based, spent when it fires). */
static atomic_t acl_dbg_fail_nth = ATOMIC_INIT(0);

int vpt1_acl_dbg_fail_nth(int n)
{
	return atomic_xchg(&acl_dbg_fail_nth, n);
}

static bool acl_alloc_fails(void)
{
	return atomic_read(&acl_dbg_fail_nth) && atomic_dec_return(&acl_dbg_fail_nth) == 0;
}
#else
static inline bool acl_alloc_fails(void)
{
	return false;
}
#endif

struct vpt1_acl_snapshot *
vpt1_acl_create(const struct vpt1_acl_domain *domain,
		const struct vpt1_acl_rule *rules, unsigned int count)
{
	struct vpt1_acl_snapshot *snapshot;
	unsigned int i, j;

	if (count > VPT1_ACL_MAX_RULES)
		return ERR_PTR(-E2BIG);
	if (!domain_valid(domain) || (count && !rules))
		return ERR_PTR(-EINVAL);
	for (i = 0; i < count; i++) {
		if (!rule_valid(domain, &rules[i]))
			return ERR_PTR(-EINVAL);
		for (j = 0; j < i; j++)
			if (rules[i].id == rules[j].id || same_match(&rules[i], &rules[j]))
				return ERR_PTR(-EEXIST);
	}
	snapshot = acl_alloc_fails() ? NULL : kzalloc(struct_size(snapshot, rules, count), GFP_KERNEL);
	if (!snapshot)
		return ERR_PTR(-ENOMEM);
	snapshot->domain = *domain;
	snapshot->count = count;
	if (count)
		memcpy(snapshot->rules, rules, count * sizeof(*rules));
	return snapshot;
}

size_t vpt1_acl_charge(const struct vpt1_acl_snapshot *snapshot)
{
	return vpt1_acl_charge_bound(snapshot->count);
}

void vpt1_acl_destroy(struct vpt1_acl_snapshot *snapshot)
{
	kfree(snapshot);
}

static bool flow_valid(const struct vpt1_acl_domain *domain,
		       const struct vpt1_acl_flow *flow)
{
	const struct vpt1_acl_owner *peer;
	__be32 local, remote;
	unsigned int i;

	if (!flow || !flow->peer_slot || flow->peer_slot > VPT1_ACL_MAX_PEERS)
		return false;
	peer = &domain->peers[flow->peer_slot - 1];
	if (flow->direction == VPT1_ACL_LOCAL_TO_PEER) {
		local = flow->source;
		remote = flow->destination;
	} else if (flow->direction == VPT1_ACL_PEER_TO_LOCAL) {
		local = flow->destination;
		remote = flow->source;
	} else {
		return false;
	}
	if (local == domain->lan_address || !host_in_prefix(local, &domain->lan))
		return false;
	for (i = 0; i < peer->count; i++)
		if (host_in_prefix(remote, &peer->prefixes[i]))
			return true;
	return false;
}

static bool rule_matches(const struct vpt1_acl_rule *rule,
			 const struct vpt1_acl_flow *flow)
{
	struct vpt1_acl_prefix source = { .address = flow->source, .length = 32 };
	struct vpt1_acl_prefix destination = { .address = flow->destination, .length = 32 };
	u16 port = be16_to_cpu(flow->destination_port);

	if (rule->peer_slot != flow->peer_slot || rule->direction != flow->direction ||
	    rule->protocol != flow->protocol || !prefix_contains(&rule->source, &source) ||
	    !prefix_contains(&rule->destination, &destination))
		return false;
	if (rule->protocol == IPPROTO_ICMP)
		return flow->icmp_type == ICMP_ECHO && !flow->icmp_code &&
		       !flow->source_port && !port;
	return port >= rule->port_first && port <= rule->port_last;
}

u32 vpt1_acl_lookup(const struct vpt1_acl_snapshot *snapshot,
		    const struct vpt1_acl_flow *flow)
{
	u32 id = 0;
	unsigned int i;

	if (!snapshot || !flow_valid(&snapshot->domain, flow))
		return 0;
	for (i = 0; i < snapshot->count; i++)
		if (rule_matches(&snapshot->rules[i], flow) &&
		    (!id || snapshot->rules[i].id < id))
			id = snapshot->rules[i].id;
	return id;
}

bool vpt1_acl_allows_rule(const struct vpt1_acl_snapshot *snapshot, u32 rule_id,
			  const struct vpt1_acl_flow *flow)
{
	unsigned int i;

	if (!snapshot || !rule_id || !flow_valid(&snapshot->domain, flow))
		return false;
	for (i = 0; i < snapshot->count; i++)
		if (snapshot->rules[i].id == rule_id)
			return rule_matches(&snapshot->rules[i], flow);
	return false;
}
