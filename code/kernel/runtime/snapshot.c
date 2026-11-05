// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/string.h>
#include <net/netlink.h>
#include <linux/unaligned.h>

#include "vpt1_snapshot.h"

/* Prefix = address (kept in network order, as the ACL stores it) and length. */
static const u8 *prefix_get(const u8 *p, struct vpt1_acl_prefix *out)
{
	memcpy(&out->address, p, 4);
	out->length = p[4];
	return p + 5;
}

static int decode_local(const u8 *p, struct vpt1_snapshot *s)
{
	s->lan_ifindex = get_unaligned_be32(p);
	s->underlay_ifindex = get_unaligned_be32(p + 4);
	p = prefix_get(p + 8, &s->domain.lan);
	p = prefix_get(p, &s->domain.underlay);
	memcpy(&s->domain.lan_address, p, 4);
	memcpy(&s->underlay_address, p + 4, 4);
	memcpy(s->local_key, p + 8, VPT1_KEY_BYTES);
	return 0;
}

static int decode_peer(const u8 *p, struct vpt1_snapshot *s)
{
	struct vpt1_cfg_peer *peer;
	unsigned int slot = p[0], i, count;

	if (slot < 1 || slot > VPT1_MAX_PEERS)
		return -EINVAL;
	peer = &s->peers[slot];
	if (peer->present)
		return -EEXIST; /* One record per slot. */
	peer->present = true;
	peer->slot = slot;
	peer->role = p[1];
	peer->pmtu = get_unaligned_be16(p + 2);
	memcpy(&peer->endpoint, p + 4, 4);
	memcpy(&peer->port, p + 8, 2);
	memcpy(peer->key, p + 10, VPT1_KEY_BYTES);
	count = p[42];
	if (!count || count > VPT1_ACL_MAX_PREFIXES)
		return -EINVAL;
	peer->owned.count = count;
	p += 43;
	for (i = 0; i < VPT1_ACL_MAX_PREFIXES; i++, p += 5) {
		if (i < count)
			prefix_get(p, &peer->owned.prefixes[i]);
		else if (p[0] | p[1] | p[2] | p[3] | p[4])
			return -EINVAL; /* Unused prefix slots are zero. */
	}
	return 0;
}

static int decode_rule(const u8 *p, struct vpt1_snapshot *s)
{
	struct vpt1_acl_rule *rule;

	if (s->rule_count >= VPT1_ACL_MAX_RULES)
		return -E2BIG;
	rule = &s->rules[s->rule_count++];
	rule->id = get_unaligned_be32(p);
	rule->peer_slot = p[4];
	rule->direction = p[5];
	rule->protocol = p[6];
	rule->port_first = get_unaligned_be16(p + 7);
	rule->port_last = get_unaligned_be16(p + 9);
	p = prefix_get(p + 11, &rule->source);
	prefix_get(p, &rule->destination);
	return 0;
}

int vpt1_snapshot_decode(const void *data, int len, struct vpt1_snapshot *out)
{
	const struct nlattr *attr;
	bool have_local = false;
	unsigned int peers = 0;
	int rem, err;

	memset(out, 0, sizeof(*out));
	if (!nla_ok(data, len) && len)
		return -EINVAL;
	nla_for_each_attr(attr, data, len, rem) {
		const u8 *p = nla_data(attr);
		int plen = nla_len(attr);

		/* Members are plain records: no nesting or byte-order flags. */
		if (attr->nla_type & (NLA_F_NESTED | NLA_F_NET_BYTEORDER))
			return -EINVAL;
		switch (nla_type(attr)) {
		case VPT1_SNAP_LOCAL:
			if (plen != VPT1_SNAP_LOCAL_LEN || have_local)
				return -EINVAL;
			have_local = true;
			err = decode_local(p, out);
			break;
		case VPT1_SNAP_PEER:
			if (plen != VPT1_SNAP_PEER_LEN || ++peers > VPT1_MAX_PEERS)
				return -EINVAL;
			err = decode_peer(p, out);
			break;
		case VPT1_SNAP_RULE:
			if (plen != VPT1_SNAP_RULE_LEN)
				return -EINVAL;
			err = decode_rule(p, out);
			break;
		default:
			return -EINVAL; /* Unknown members fail the whole request. */
		}
		if (err)
			return err;
	}
	if (rem || !have_local)
		return -EINVAL;
	return 0;
}
