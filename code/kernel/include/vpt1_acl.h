/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_ACL_H
#define VPT1_ACL_H

#include <linux/types.h>

#define VPT1_ACL_MAX_RULES 128U
#define VPT1_ACL_MAX_PEERS 4U
#define VPT1_ACL_MAX_PREFIXES 4U

/* Internal values, not a wire/Netlink ABI or the Noise exchange role. */
enum vpt1_acl_direction {
	VPT1_ACL_LOCAL_TO_PEER = 1,
	VPT1_ACL_PEER_TO_LOCAL = 2,
};

struct vpt1_acl_prefix {
	__be32 address;
	u8 length;
};

struct vpt1_acl_owner {
	struct vpt1_acl_prefix prefixes[VPT1_ACL_MAX_PREFIXES];
	u8 count; /* Zero means this slot is not configured. */
};

/* Value-only projection of the shared configuration, never a peer object. */
struct vpt1_acl_domain {
	struct vpt1_acl_prefix lan;
	struct vpt1_acl_prefix underlay;
	__be32 lan_address;
	struct vpt1_acl_owner peers[VPT1_ACL_MAX_PEERS];
};

struct vpt1_acl_rule {
	u32 id; /* Nonzero, stable identity; never retain a pointer to this rule. */
	struct vpt1_acl_prefix source;
	struct vpt1_acl_prefix destination;
	u16 port_first; /* Host order; both zero for diagnostic echo-request. */
	u16 port_last;
	u8 peer_slot; /* Shared slot map, 1..4; not a reordered array index. */
	u8 direction;
	u8 protocol; /* IPPROTO_TCP, IPPROTO_UDP or IPPROTO_ICMP (echo only). */
};

/* Validated, normalized ORIGINAL initiating tuple, not a reply's reversed tuple. */
struct vpt1_acl_flow {
	__be32 source;
	__be32 destination;
	__be16 source_port;
	__be16 destination_port;
	u8 peer_slot;
	u8 direction;
	u8 protocol;
	u8 icmp_type;
	u8 icmp_code;
};

struct vpt1_acl_snapshot;

/*
 * Process context only: validate the complete value projection, then allocate
 * and copy it. Returns an owned immutable snapshot or ERR_PTR(-EINVAL/-E2BIG/
 * -EEXIST/-ENOMEM). Invalid input never modifies an existing snapshot.
 * The caller supplies kernel-owned decoded arrays, stable for the whole call.
 * No raw wire/native-struct decoding, endpoint/identity validation or publication
 * happens here. Reserve charge_bound(count) in the shared budget before create;
 * release that reservation on failure. No allocations occur during lookup.
 */
struct vpt1_acl_snapshot *
vpt1_acl_create(const struct vpt1_acl_domain *domain,
		const struct vpt1_acl_rule *rules, unsigned int count);
size_t vpt1_acl_charge_bound(unsigned int count); /* Zero for an invalid count. */
size_t vpt1_acl_charge(const struct vpt1_acl_snapshot *snapshot);

/*
 * The shared cfg exclusively owns this allocation. Destroy from its non-sleeping
 * RCU reclamation callback after unpublication and the grace period, or after an
 * equivalent reader drain. Queued work must not retain cfg/ACL pointers outside
 * an RCU read-side section. This helper does not start a second RCU lifetime.
 */
void vpt1_acl_destroy(struct vpt1_acl_snapshot *snapshot);

/*
 * Read-only, bounded, non-sleeping. Borrow snapshot/flow for this call; caller
 * holds their lifetime and reloads CURRENT policy under the peer admission lock.
 * Return the smallest matching stable ID, or zero (default deny). Listing order
 * cannot change permission. This is only an initiating ACL predicate: no witness,
 * conntrack, source authentication, eligibility or final-output grant is minted.
 */
u32 vpt1_acl_lookup(const struct vpt1_acl_snapshot *snapshot,
		    const struct vpt1_acl_flow *flow);

/* Revalidate the witness's ORIGINAL tuple and exact saved ID against CURRENT ACL.
 * Never switch an existing witness to another matching rule after revocation.
 */
bool vpt1_acl_allows_rule(const struct vpt1_acl_snapshot *snapshot, u32 rule_id,
			  const struct vpt1_acl_flow *flow);

#ifdef VPT1_DEBUG
/* Test knob, compiled out of release builds: fail the Nth snapshot allocation (1-based); returns the previous
 * remaining count, so setting 0 afterwards tells whether the armed failure fired (0 means it did). */
int vpt1_acl_dbg_fail_nth(int n);
#endif

#endif /* VPT1_ACL_H */
