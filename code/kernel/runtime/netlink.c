// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic Netlink endpoint "vptunnel" (ABI v1, protocol/netlink-v1.json).
 * Contract: docs/AY2_CONTRACT.md sections 2 and 3. Every operation needs
 * CAP_NET_ADMIN in the owning user namespace (GENL_UNS_ADMIN_PERM) and the
 * gateway is always resolved from the request's network namespace.
 */
#include <linux/build_bug.h>
#include <linux/ktime.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>

#include "vpt1_gateway.h"
#include "vpt1_generation.h"
#include "vpt1_netlink_ids.h"
#include "vpt1_runtime.h"
#include "vpt1_snapshot.h"
#include "vpt1_wire.h"

#define A(name) BIT(VPT1_ATTR_##name)

/* Nested COUNTERS uses index + 1. Never shift published member IDs. */
static_assert(VPT1_R_COUNT == 14);
static_assert(VPT1_E_LEASE_EXPIRED == 14);
static_assert(VPT1_E_RX_ECN_ANOMALY == 35);
static_assert(VPT1_E_TX_PING == 36);
static_assert(VPT1_E_RX_PING == 37);
static_assert(VPT1_E_TX_PONG == 38);
static_assert(VPT1_E_RX_PONG == 39);
static_assert(VPT1_E_RX_CONTROL_DROPPED == 40);
static_assert(VPT1_E_LIVENESS_LOST == 41);
static_assert(VPT1_E_ACTIVATED == 42);
static_assert(VPT1_E_REKEY_NEEDED == 48);
static_assert(VPT1_R_SIZE == 49);
static_assert(VPT1_E_FRAG_QUEUED == 50);
static_assert(VPT1_E_TX_GSO_LIMIT == 56);
static_assert(VPT1_E_COUNT == 57);

static struct genl_family vpt1_genl_family;

static const struct nla_policy vpt1_policy[VPT1_ATTR_REQUEST_ID + 1] = {
	[VPT1_ATTR_IFINDEX] = { .type = NLA_U32 },
	[VPT1_ATTR_CONTROLLER_ID] = NLA_POLICY_EXACT_LEN(VPT1_ATTR_CONTROLLER_ID_LEN),
	[VPT1_ATTR_CONFIG_REVISION] = { .type = NLA_U64 },
	[VPT1_ATTR_PEER_SLOT] = NLA_POLICY_RANGE(NLA_U16, 1, VPT1_MAX_PEERS),
	[VPT1_ATTR_PEER_INCARNATION] =
		NLA_POLICY_EXACT_LEN(VPT1_ATTR_PEER_INCARNATION_LEN),
	[VPT1_ATTR_ATTEMPT_ID] = NLA_POLICY_EXACT_LEN(VPT1_ATTR_ATTEMPT_ID_LEN),
	[VPT1_ATTR_HANDSHAKE_HASH] =
		NLA_POLICY_EXACT_LEN(VPT1_ATTR_HANDSHAKE_HASH_LEN),
	[VPT1_ATTR_LOCAL_RX] = { .type = NLA_U64 },
	[VPT1_ATTR_REMOTE_RX] = { .type = NLA_U64 },
	[VPT1_ATTR_TX_KEY] = NLA_POLICY_EXACT_LEN(VPT1_ATTR_TX_KEY_LEN),
	[VPT1_ATTR_RX_KEY] = NLA_POLICY_EXACT_LEN(VPT1_ATTR_RX_KEY_LEN),
	[VPT1_ATTR_MODE] = { .type = NLA_U8 },
	[VPT1_ATTR_PARENT_HASH] = NLA_POLICY_EXACT_LEN(VPT1_ATTR_PARENT_HASH_LEN),
	[VPT1_ATTR_EXCHANGE_BYTES] =
		NLA_POLICY_MAX_LEN(VPT1_ATTR_EXCHANGE_BYTES_MAX_LEN),
	[VPT1_ATTR_SNAPSHOT] = { .type = NLA_NESTED },
	[VPT1_ATTR_INTENT] = { .type = NLA_U8 },
	[VPT1_ATTR_STATUS] = { .type = NLA_NESTED },
	[VPT1_ATTR_COUNTERS] = { .type = NLA_NESTED },
	[VPT1_ATTR_REQUEST_ID] = { .type = NLA_U64 },
};

struct vpt1_reply {
	bool has_status;
	bool has_counters;
	bool has_attempt;
	u8 attempt_id[VPT1_ATTEMPT_ID_LEN];
	u64 attempt_rx;
	struct vpt1_status status;
	u64 counters[VPT1_E_COUNT];
};

typedef int (*vpt1_op_fn)(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
			  struct vpt1_reply *reply);

/*
 * Required and optional attributes per command: a missing required attribute or
 * any attribute outside the two sets fails the whole request with no effect.
 * PROPOSED: this is the per-command table the ABI still needs (AY2_RUNTIME.md).
 */
struct vpt1_spec {
	u32 required;
	u32 optional;
	vpt1_op_fn fn;
};

#define OWNER (A(IFINDEX) | A(CONTROLLER_ID))

static int op_claim(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		    struct vpt1_reply *reply)
{
	u64 epoch;

	return vpt1_gw_claim(gw, nla_data(info->attrs[VPT1_ATTR_CONTROLLER_ID]),
			     info->snd_portid, now_ns, &epoch);
}

static int op_renew(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		    struct vpt1_reply *reply)
{
	return vpt1_gw_renew(gw, nla_data(info->attrs[VPT1_ATTR_CONTROLLER_ID]),
			     info->snd_portid,
			     nla_get_u64(info->attrs[VPT1_ATTR_CONFIG_REVISION]),
			     now_ns);
}

static int op_validate(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		       struct vpt1_reply *reply)
{
	const struct nlattr *attr = info->attrs[VPT1_ATTR_SNAPSHOT];
	struct vpt1_snapshot *snap;
	int err;

	if (nla_len(attr) > VPT1_ATTR_SNAPSHOT_MAX_LEN) {
		NL_SET_ERR_MSG_ATTR(info->extack, attr, "snapshot too large");
		return -EMSGSIZE;
	}
	snap = kzalloc(sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;
	err = vpt1_snapshot_decode(nla_data(attr), nla_len(attr), snap);
	if (err) {
		NL_SET_ERR_MSG_ATTR(info->extack, attr, "invalid snapshot");
		goto out;
	}
	err = vpt1_gw_validate(gw, nla_get_u64(info->attrs[VPT1_ATTR_CONFIG_REVISION]),
			       snap);
out:
	kfree(snap);
	return err;
}

static int op_commit(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		     struct vpt1_reply *reply)
{
	bool disruptive;

	return vpt1_gw_commit(gw,
			      nla_get_u64(info->attrs[VPT1_ATTR_CONFIG_REVISION]),
			      now_ns, &disruptive);
}

static int op_stage(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		    struct vpt1_reply *reply)
{
	struct nlattr *tx = info->attrs[VPT1_ATTR_TX_KEY];
	struct nlattr *rx = info->attrs[VPT1_ATTR_RX_KEY];
	struct vpt1_incarnation inc;
	struct vpt1_generation *gen;
	u8 *attempt = nla_data(info->attrs[VPT1_ATTR_ATTEMPT_ID]);
	u16 slot = nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]);
	u64 local_rx = nla_get_u64(info->attrs[VPT1_ATTR_LOCAL_RX]);
	u64 remote_rx = nla_get_u64(info->attrs[VPT1_ATTR_REMOTE_RX]);
	u8 *hash = nla_data(info->attrs[VPT1_ATTR_HANDSHAKE_HASH]);
	u8 mode = nla_get_u8(info->attrs[VPT1_ATTR_MODE]);
	int err;

	if (nla_get_u8(info->attrs[VPT1_ATTR_MODE]) > 1) {
		NL_SET_ERR_MSG_ATTR(info->extack, info->attrs[VPT1_ATTR_MODE],
				    "unknown mode");
		return -EINVAL;
	}
	memcpy(inc.id, nla_data(info->attrs[VPT1_ATTR_PEER_INCARNATION]),
	       VPT1_ID_LEN);
	/* The keys may only complete the attempt the kernel issued, and use the locator it issued. */
	err = vpt1_gw_attempt_check(gw, slot, &inc, attempt, local_rx, now_ns, false);
	if (!err) {
		/*
		 * A repeated, identical installation is acknowledged and changes nothing (the keys are not
		 * installed twice, no counter is reset); a conflicting reuse of the attempt is refused.
		 */
		err = vpt1_gw_stage_duplicate(gw, slot, &inc, attempt, local_rx, remote_rx, hash);
		if (err > 0) {
			memzero_explicit(nla_data(tx), nla_len(tx));
			memzero_explicit(nla_data(rx), nla_len(rx));
			return 0;
		}
		if (!err)
			err = vpt1_gw_attempt_check(gw, slot, &inc, attempt, local_rx, now_ns, true);
	}
	if (err) {
		memzero_explicit(nla_data(tx), nla_len(tx));
		memzero_explicit(nla_data(rx), nla_len(rx));
		NL_SET_ERR_MSG(info->extack, "no such pending attempt (begin one first)");
		return err;
	}
	gen = vpt1_generation_create(nla_data(tx), nla_data(rx), now_ns);
	/* Keys are write-only: wipe the request's copy whatever happened. */
	memzero_explicit(nla_data(tx), nla_len(tx));
	memzero_explicit(nla_data(rx), nla_len(rx));
	if (IS_ERR(gen)) {
		NL_SET_ERR_MSG(info->extack, "key generation rejected");
		return PTR_ERR(gen);
	}
	err = vpt1_gw_stage_generation(gw, slot, &inc, gen, local_rx, remote_rx, hash, mode, attempt,
				       info->attrs[VPT1_ATTR_PARENT_HASH] ?
					       nla_data(info->attrs[VPT1_ATTR_PARENT_HASH]) : NULL);
	if (err)
		vpt1_generation_put(gen);
	else
		vpt1_gw_attempt_done(gw, slot, attempt, now_ns);
	return err;
}

static int op_rekey_request(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
			    struct vpt1_reply *reply)
{
	struct vpt1_incarnation inc;

	memcpy(inc.id, nla_data(info->attrs[VPT1_ATTR_PEER_INCARNATION]), VPT1_ID_LEN);
	return vpt1_gw_rekey_request(gw, nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]), &inc,
				     nla_data(info->attrs[VPT1_ATTR_ATTEMPT_ID]), now_ns);
}

static int op_begin_attempt(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
			    struct vpt1_reply *reply)
{
	struct vpt1_incarnation inc;
	int err;

	memcpy(inc.id, nla_data(info->attrs[VPT1_ATTR_PEER_INCARNATION]), VPT1_ID_LEN);
	err = vpt1_gw_attempt_begin(gw, nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]), &inc,
				    info->attrs[VPT1_ATTR_ATTEMPT_ID] ?
					nla_data(info->attrs[VPT1_ATTR_ATTEMPT_ID]) : NULL,
				    now_ns, reply->attempt_id, &reply->attempt_rx);
	if (!err)
		reply->has_attempt = true;
	return err;
}

static int op_unsupported(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
			  struct vpt1_reply *reply)
{
	/* No UDP transport exists yet (AY5): accepted syntax, no effect. */
	NL_SET_ERR_MSG(info->extack, "no transport in this build");
	return -EOPNOTSUPP;
}

/* An exchange message is exactly an INIT (224) or a RESPONSE (176) UDP payload. */
static bool exchange_len_ok(size_t len)
{
	return len == VPT1_INIT_LEN || len == VPT1_RESPONSE_LEN;
}

static int op_exchange_tx(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
			  struct vpt1_reply *reply)
{
	const struct nlattr *bytes = info->attrs[VPT1_ATTR_EXCHANGE_BYTES];

	struct vpt1_datagram view;
	int err;

	if (!exchange_len_ok(nla_len(bytes)) ||
	    vpt1_parse_datagram(nla_data(bytes), nla_len(bytes), VPT1_INNER_CAP_MAX, &view) ||
	    (view.kind != VPT1_INIT && view.kind != VPT1_RESPONSE)) {
		NL_SET_ERR_MSG_ATTR(info->extack, bytes,
				    "exchange bytes must be a whole INIT (224) or RESPONSE (176)");
		return -EINVAL;
	}
	/* Only the pending attempt's own INIT or RESPONSE may leave through the kernel socket. */
	err = vpt1_gw_attempt_check(gw, nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]), NULL,
				    view.attempt, 0, now_ns, false);
	if (err) {
		NL_SET_ERR_MSG(info->extack, "exchange bytes do not belong to a pending attempt");
		return err;
	}
	return vpt1_dp_send_exchange(gw, nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]),
				     nla_data(bytes), nla_len(bytes));
}

static int op_disable(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		      struct vpt1_reply *reply)
{
	struct vpt1_incarnation inc;

	memcpy(inc.id, nla_data(info->attrs[VPT1_ATTR_PEER_INCARNATION]),
	       VPT1_ID_LEN);
	return vpt1_gw_disable_peer(gw, nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]),
				    &inc);
}

static int op_revoke(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		     struct vpt1_reply *reply)
{
	struct vpt1_incarnation inc;

	memcpy(inc.id, nla_data(info->attrs[VPT1_ATTR_PEER_INCARNATION]),
	       VPT1_ID_LEN);
	return vpt1_gw_revoke_peer(gw, nla_get_u16(info->attrs[VPT1_ATTR_PEER_SLOT]),
				   &inc);
}

static int op_status(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		     struct vpt1_reply *reply)
{
	reply->has_status = true;
	return vpt1_gw_status(gw, now_ns, &reply->status);
}

static int op_counters(struct vpt1_gw *gw, struct genl_info *info, u64 now_ns,
		       struct vpt1_reply *reply)
{
	reply->has_counters = true;
	vpt1_gw_counters(gw, reply->counters);
	return 0;
}

static const struct vpt1_spec vpt1_specs[] = {
	[VPT1_CMD_CLAIM] = { OWNER, A(REQUEST_ID), op_claim },
	[VPT1_CMD_RENEW] = { OWNER | A(CONFIG_REVISION), A(REQUEST_ID), op_renew },
	[VPT1_CMD_VALIDATE] = { OWNER | A(CONFIG_REVISION) | A(SNAPSHOT),
				A(REQUEST_ID), op_validate },
	[VPT1_CMD_COMMIT_CONFIG] = { OWNER | A(CONFIG_REVISION), A(REQUEST_ID),
				     op_commit },
	[VPT1_CMD_STAGE_GENERATION] = { OWNER | A(PEER_SLOT) | A(PEER_INCARNATION) |
					A(ATTEMPT_ID) | A(HANDSHAKE_HASH) |
					A(LOCAL_RX) | A(REMOTE_RX) | A(TX_KEY) |
					A(RX_KEY) | A(MODE),
					A(PARENT_HASH) | A(REQUEST_ID), op_stage },
	[VPT1_CMD_EXCHANGE_TX] = { OWNER | A(PEER_SLOT) | A(EXCHANGE_BYTES),
				   A(REQUEST_ID), op_exchange_tx },
	[VPT1_CMD_CONTROL_INTENT] = { OWNER | A(PEER_SLOT) | A(INTENT),
				      A(REQUEST_ID), op_unsupported },
	[VPT1_CMD_DISABLE] = { OWNER | A(PEER_SLOT) | A(PEER_INCARNATION),
			       A(REQUEST_ID), op_disable },
	[VPT1_CMD_REVOKE] = { OWNER | A(PEER_SLOT) | A(PEER_INCARNATION),
			      A(REQUEST_ID), op_revoke },
	[VPT1_CMD_STATUS] = { A(IFINDEX), A(REQUEST_ID), op_status },
	[VPT1_CMD_COUNTERS] = { A(IFINDEX), A(REQUEST_ID), op_counters },
	[VPT1_CMD_REMOVE_GATEWAY] = { OWNER, A(REQUEST_ID), NULL },
	[VPT1_CMD_BEGIN_ATTEMPT] = { OWNER | A(PEER_SLOT) | A(PEER_INCARNATION),
				     A(ATTEMPT_ID) | A(REQUEST_ID), op_begin_attempt },
	[VPT1_CMD_REKEY_REQUEST] = { OWNER | A(PEER_SLOT) | A(PEER_INCARNATION) | A(ATTEMPT_ID),
				     A(REQUEST_ID), op_rekey_request },
};

static int check_attrs(struct genl_info *info, const struct vpt1_spec *spec)
{
	unsigned int id;

	for (id = 1; id <= VPT1_ATTR_REQUEST_ID; id++) {
		bool present = info->attrs[id];

		if (present && !((spec->required | spec->optional) & BIT(id))) {
			NL_SET_ERR_MSG_ATTR(info->extack, info->attrs[id],
					    "attribute not valid for this command");
			return -EINVAL;
		}
		if (!present && (spec->required & BIT(id))) {
			NL_SET_ERR_MSG(info->extack, "required attribute missing");
			return -EINVAL;
		}
	}
	return 0;
}

static int put_status(struct sk_buff *skb, const struct vpt1_status *st)
{
	struct nlattr *nest = nla_nest_start(skb, VPT1_ATTR_STATUS);
	unsigned int i;

	if (!nest)
		return -EMSGSIZE;
	if (nla_put_u8(skb, VPT1_STATUS_OWNED, st->owned) ||
	    nla_put_u8(skb, VPT1_STATUS_STOPPING, st->stopping) ||
	    nla_put(skb, VPT1_STATUS_OWNER_EPOCH, sizeof(u64), &st->owner_epoch) ||
	    nla_put(skb, VPT1_STATUS_LEASE_MS, sizeof(u64), &st->lease_remaining_ms) ||
	    nla_put(skb, VPT1_STATUS_REVISION, sizeof(u64), &st->revision))
		goto cancel;
	if (st->has_staged &&
	    nla_put(skb, VPT1_STATUS_STAGED_REVISION, sizeof(u64),
		    &st->staged_revision))
		goto cancel;
	for (i = 0; i < st->peer_count; i++) {
		const struct vpt1_status_peer *p = &st->peers[i];
		struct nlattr *peer = nla_nest_start(skb, VPT1_STATUS_PEER);

		if (!peer)
			goto cancel;
		/* Keys, counters and generation contents are never reported. */
		if (nla_put_u16(skb, VPT1_STATUS_PEER_SLOT, p->slot) ||
		    nla_put_u8(skb, VPT1_STATUS_PEER_STATE, p->state) ||
		    nla_put_u8(skb, VPT1_STATUS_PEER_HAS_GEN, p->has_generation) ||
		    nla_put_u16(skb, VPT1_STATUS_PEER_PMTU, p->pmtu) ||
		    nla_put(skb, VPT1_STATUS_PEER_ENDPOINT, 4, &p->endpoint) ||
		    nla_put(skb, VPT1_STATUS_PEER_PORT, 2, &p->port) ||
		    nla_put(skb, VPT1_STATUS_PEER_INCARNATION, VPT1_ID_LEN,
			    p->incarnation.id)) {
			nla_nest_cancel(skb, peer);
			goto cancel;
		}
		nla_nest_end(skb, peer);
	}
	nla_nest_end(skb, nest);
	return 0;
cancel:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int put_counters(struct sk_buff *skb, const u64 *counters)
{
	struct nlattr *nest = nla_nest_start(skb, VPT1_ATTR_COUNTERS);
	unsigned int i;

	if (!nest)
		return -EMSGSIZE;
	for (i = 0; i < VPT1_E_COUNT; i++) {
		if (nla_put(skb, i + 1, sizeof(u64), &counters[i])) {
			nla_nest_cancel(skb, nest);
			return -EMSGSIZE;
		}
	}
	nla_nest_end(skb, nest);
	return 0;
}

/* Every success is answered with one reply that echoes REQUEST_ID. */
static int send_reply(struct genl_info *info, const struct vpt1_reply *reply)
{
	struct sk_buff *skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	void *hdr;
	int err;

	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put_reply(skb, info, &vpt1_genl_family, 0, info->genlhdr->cmd);
	if (!hdr) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}
	err = 0;
	if (info->attrs[VPT1_ATTR_REQUEST_ID])
		err = nla_put(skb, VPT1_ATTR_REQUEST_ID, sizeof(u64),
			      nla_data(info->attrs[VPT1_ATTR_REQUEST_ID]));
	if (!err && reply->has_status)
		err = put_status(skb, &reply->status);
	if (!err && reply->has_counters)
		err = put_counters(skb, reply->counters);
	if (!err && reply->has_attempt)
		err = nla_put(skb, VPT1_ATTR_ATTEMPT_ID, VPT1_ATTEMPT_ID_LEN, reply->attempt_id) ||
		      nla_put(skb, VPT1_ATTR_LOCAL_RX, sizeof(u64), &reply->attempt_rx);
	if (err) {
		nlmsg_free(skb);
		return err;
	}
	genlmsg_end(skb, hdr);
	return genlmsg_reply(skb, info);
}

static int vpt1_remove_gateway(struct net *net, struct genl_info *info, u32 ifindex)
{
	struct vpt1_gw *gw;
	int err;

	/* RTNL first, then the lookup: the same order as ensure and dellink. */
	rtnl_lock();
	gw = vpt1_net_gw_get(net);
	if (!gw || vpt1_gw_ifindex(gw) != ifindex) {
		rtnl_unlock();
		if (gw)
			vpt1_gw_put(gw);
		return -ENODEV;
	}
	err = vpt1_gw_check_owner(gw, nla_data(info->attrs[VPT1_ATTR_CONTROLLER_ID]),
				  info->snd_portid, ktime_get_boottime_ns());
	if (err) {
		rtnl_unlock();
		vpt1_gw_count_rejected(gw);
		vpt1_gw_put(gw);
		return err;
	}
	vpt1_net_gw_unpublish(net, gw);
	rtnl_unlock();
	/* Admission closes first; destroy waits for other handlers, not for us. */
	vpt1_gw_destroy(gw, 1);
	vpt1_gw_put(gw);
	return 0;
}

static int vpt1_doit(struct sk_buff *skb, struct genl_info *info)
{
	u8 cmd = info->genlhdr->cmd;
	const struct vpt1_spec *spec;
	struct vpt1_reply *reply;
	struct net *net = genl_info_net(info);
	struct vpt1_gw *gw;
	u32 ifindex;
	u64 now_ns;
	int err;

	if (cmd >= ARRAY_SIZE(vpt1_specs) || !vpt1_specs[cmd].required)
		return -EOPNOTSUPP;
	spec = &vpt1_specs[cmd];
	err = check_attrs(info, spec);
	if (err)
		return err;
	ifindex = nla_get_u32(info->attrs[VPT1_ATTR_IFINDEX]);
	if (cmd == VPT1_CMD_REMOVE_GATEWAY) {
		err = vpt1_remove_gateway(net, info, ifindex);
		if (err)
			return err;
		return send_reply(info, &(struct vpt1_reply){});
	}
	/* Device removal holds RTNL while draining handler references. COMMIT
	 * must acquire RTNL BEFORE taking one, never wait for RTNL with it held.
	 */
	if (cmd == VPT1_CMD_COMMIT_CONFIG)
		rtnl_lock();

	if (cmd == VPT1_CMD_CLAIM) {
		rtnl_lock();
		gw = vpt1_net_gw_ensure(net, ifindex);
		rtnl_unlock();
		if (IS_ERR(gw))
			return PTR_ERR(gw);
	} else {
		gw = vpt1_net_gw_get(net);
		if (!gw) {
			err = -ENODEV;
			goto out_rtnl;
		}
		if (vpt1_gw_ifindex(gw) != ifindex) {
			vpt1_gw_put(gw);
			err = -ENODEV;
			goto out_rtnl;
		}
	}

	reply = kzalloc(sizeof(*reply), GFP_KERNEL);
	if (!reply) {
		vpt1_gw_put(gw);
		err = -ENOMEM;
		goto out_rtnl;
	}
	vpt1_gw_lock(gw);
	now_ns = ktime_get_boottime_ns();
	/* Owner-gated: the exact triple of controller ID, portid and namespace. */
	err = 0;
	if (cmd != VPT1_CMD_CLAIM && (spec->required & A(CONTROLLER_ID)))
		err = vpt1_gw_check_owner(gw,
					  nla_data(info->attrs[VPT1_ATTR_CONTROLLER_ID]),
					  info->snd_portid, now_ns);
	if (!err)
		err = spec->fn(gw, info, now_ns, reply);
	vpt1_gw_unlock(gw);
	if (cmd == VPT1_CMD_COMMIT_CONFIG)
		rtnl_unlock();
	if (err)
		vpt1_gw_count_rejected(gw);
	else
		err = send_reply(info, reply);
	kfree(reply);
	vpt1_gw_put(gw);
	return err;
out_rtnl:
	if (cmd == VPT1_CMD_COMMIT_CONFIG)
		rtnl_unlock();
	return err;
}

static const struct genl_small_ops vpt1_ops[] = {
#define OP(c) { .cmd = c, .doit = vpt1_doit, .flags = GENL_UNS_ADMIN_PERM }
	OP(VPT1_CMD_CLAIM), OP(VPT1_CMD_RENEW), OP(VPT1_CMD_VALIDATE),
	OP(VPT1_CMD_COMMIT_CONFIG), OP(VPT1_CMD_STAGE_GENERATION),
	OP(VPT1_CMD_EXCHANGE_TX), OP(VPT1_CMD_CONTROL_INTENT),
	OP(VPT1_CMD_DISABLE), OP(VPT1_CMD_REVOKE), OP(VPT1_CMD_STATUS),
	OP(VPT1_CMD_COUNTERS), OP(VPT1_CMD_REMOVE_GATEWAY), OP(VPT1_CMD_BEGIN_ATTEMPT),
	OP(VPT1_CMD_REKEY_REQUEST),
#undef OP
};

static struct genl_family vpt1_genl_family = {
	.name = VPT1_GENL_FAMILY,
	.version = VPT1_GENL_VERSION,
	.maxattr = VPT1_ATTR_REQUEST_ID,
	.policy = vpt1_policy,
	.netnsok = true,
	.parallel_ops = true,
	.resv_start_op = VPT1_CMD_CLAIM, /* Strict validation for every command. */
	.module = THIS_MODULE,
	.small_ops = vpt1_ops,
	.n_small_ops = ARRAY_SIZE(vpt1_ops),
};

/* EXCHANGE_RX: unicast to the owner only; INIT/RESPONSE bytes, never keys. */
int vpt1_netlink_exchange_rx(struct net *net, struct vpt1_gw *gw, u8 slot,
			     const struct vpt1_incarnation *inc, const void *bytes,
			     size_t len)
{
	struct sk_buff *skb;
	void *hdr;
	u32 portid;
	int err;

	if (!exchange_len_ok(len))
		return -EMSGSIZE; /* Only whole INIT/RESPONSE payloads go to the owner. */
	err = vpt1_gw_owner_portid(gw, &portid);
	if (err)
		return err;
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	hdr = genlmsg_put(skb, 0, 0, &vpt1_genl_family, 0, VPT1_CMD_EXCHANGE_RX);
	if (!hdr || nla_put_u16(skb, VPT1_ATTR_PEER_SLOT, slot) ||
	    nla_put(skb, VPT1_ATTR_PEER_INCARNATION, VPT1_ID_LEN, inc->id) ||
	    nla_put(skb, VPT1_ATTR_EXCHANGE_BYTES, len, bytes)) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}
	genlmsg_end(skb, hdr);
	/* A full socket drops the event; the controller's retry timers recover. */
	return genlmsg_unicast(net, skb, portid);
}

/* REKEY_GRANTED and REKEY_NEEDED: unicast to the owner only; identifiers, never keys. */
void vpt1_netlink_rekey_event(struct net *net, struct vpt1_gw *gw, u8 slot,
			      const struct vpt1_incarnation *inc, enum vpt1_ctl_event event,
			      const u8 *attempt)
{
	struct sk_buff *skb;
	void *hdr;
	u32 portid;

	if (vpt1_gw_owner_portid(gw, &portid))
		return;
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return;
	hdr = genlmsg_put(skb, 0, 0, &vpt1_genl_family, 0,
			  event == VPT1_EVT_REKEY_GRANTED ? VPT1_CMD_REKEY_GRANTED :
							    VPT1_CMD_REKEY_NEEDED);
	if (!hdr || nla_put_u16(skb, VPT1_ATTR_PEER_SLOT, slot) ||
	    nla_put(skb, VPT1_ATTR_PEER_INCARNATION, VPT1_ID_LEN, inc->id) ||
	    (event == VPT1_EVT_REKEY_GRANTED && attempt &&
	     nla_put(skb, VPT1_ATTR_ATTEMPT_ID, VPT1_ATTEMPT_ID_LEN, attempt))) {
		nlmsg_free(skb);
		return;
	}
	genlmsg_end(skb, hdr);
	/* A full socket drops the event; the controller's own timers recover. */
	genlmsg_unicast(net, skb, portid);
}

static int vpt1_netlink_event(struct notifier_block *nb, unsigned long state,
			      void *ptr)
{
	struct netlink_notify *n = ptr;
	struct vpt1_gw *gw;

	/* The same numeric portid exists on every protocol: Generic Netlink only. */
	if (state != NETLINK_URELEASE || n->protocol != NETLINK_GENERIC)
		return NOTIFY_DONE;
	gw = vpt1_net_gw_get(n->net);
	if (!gw)
		return NOTIFY_DONE;
	vpt1_gw_owner_released(gw, n->portid);
	vpt1_gw_put(gw);
	return NOTIFY_DONE;
}

static struct notifier_block vpt1_netlink_nb = {
	.notifier_call = vpt1_netlink_event,
};

int vpt1_netlink_init(void)
{
	int err = netlink_register_notifier(&vpt1_netlink_nb);

	if (err)
		return err;
	err = genl_register_family(&vpt1_genl_family);
	if (err)
		netlink_unregister_notifier(&vpt1_netlink_nb);
	return err;
}

void vpt1_netlink_exit(void)
{
	/* Notifier first, then the family (which waits for in-flight handlers). */
	netlink_unregister_notifier(&vpt1_netlink_nb);
	genl_unregister_family(&vpt1_genl_family);
}
