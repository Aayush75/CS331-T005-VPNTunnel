// SPDX-License-Identifier: GPL-2.0-only
/*
 * Data path: one UDP socket per gateway, VPT1 RECORD seal/open with the directional
 * generations, and vpn0 transmit/receive through the admission API.
 *
 * What it is: IPv4 TCP, UDP and ICMP echo between two statically keyed gateways,
 * every packet authenticated, replay-checked and policy-checked before it is handed
 * on. INIT/RESPONSE are relayed to the controller; the gateway core owns generation
 * selection/promotion. The ACL is stateless (see vpt1_admit_packet()); flow witnesses
 * are not live. AEAD is synchronous.
 *
 * HR3/HR4 live adapter (docs/HR3_LIVE.md, docs/HR4_ERRORS.md):
 *  - IPv4 fragments, from the LAN toward the tunnel and out of it, are queued as bounded jobs,
 *    normalized by a per-peer cache in the gateway worker and only then policy-checked. A
 *    completed LAN-side datagram is sent as ordinary records (re-fragmented to the pair cap if it
 *    is larger); a completed tunnel-side one is delivered once on vpn0.
 *  - ICMP destination-unreachable and time-exceeded errors cross only when their quotation
 *    matches a recent packet of the opposite direction, and the saved rule still allows it.
 *  - One transmit batch (one GSO aggregate) is bounded to 64 packets.
 */
#include <crypto/aead.h>
#include <linux/delay.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/netdevice.h>
#include <linux/moduleparam.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/unaligned.h>
#include <net/ip.h>
#include <net/net_namespace.h>
#include <net/route.h>
#include <net/sock.h>
#include <net/udp.h>
#include <net/udp_tunnel.h>

#include "vpt1_error.h"
#include "vpt1_fragment.h"
#include "vpt1_gateway.h"
#include "vpt1_generation.h"
#include "vpt1_ip.h"
#include "vpt1_runtime.h"
#include "vpt1_wire.h"

#define VPT1_UDP_PORT 51888

#ifdef VPT1_DEBUG
/* Bounded delay/observation only, never an authorization bypass. Allows the
 * disposable-VM oracle to commit policy after crypto but before final output.
 */
static unsigned int tx_test_delay_us;
static atomic_t tx_test_seen = ATOMIC_INIT(0);
module_param(tx_test_delay_us, uint, 0600);
static unsigned int rx_test_delay_us;
static atomic_t rx_test_seen = ATOMIC_INIT(0);
module_param(rx_test_delay_us, uint, 0600);
static unsigned int control_test_netns;
static unsigned int control_test_delay_us;
static atomic_t control_test_seen = ATOMIC_INIT(0);
module_param(control_test_netns, uint, 0600);
module_param(control_test_delay_us, uint, 0600);

static int tx_test_seen_get(char *buffer, const struct kernel_param *param)
{
	return scnprintf(buffer, 32, "%d\n", atomic_read(param->arg));
}

static const struct kernel_param_ops tx_test_seen_ops = { .get = tx_test_seen_get };
module_param_cb(tx_test_seen, &tx_test_seen_ops, &tx_test_seen, 0400);
module_param_cb(rx_test_seen, &tx_test_seen_ops, &rx_test_seen, 0400);
module_param_cb(control_test_seen, &tx_test_seen_ops, &control_test_seen, 0400);

static void packet_test_pause(unsigned int requested, atomic_t *seen)
{
	unsigned int delay = min_t(unsigned int, requested, 100000);

	if (delay) {
		atomic_inc(seen);
		udelay(delay);
	}
}

static void tx_test_pause(void)
{
	packet_test_pause(READ_ONCE(tx_test_delay_us), &tx_test_seen);
}

static void rx_test_pause(void)
{
	packet_test_pause(READ_ONCE(rx_test_delay_us), &rx_test_seen);
}

static void control_test_pause(struct vpt1_gw *gw, u8 subtype)
{
	if (subtype == VPT1_PONG &&
	    READ_ONCE(control_test_netns) == vpt1_gw_net(gw)->ns.inum)
		packet_test_pause(READ_ONCE(control_test_delay_us), &control_test_seen);
}
#else
static void tx_test_pause(void)
{
}

static void rx_test_pause(void)
{
}

static void control_test_pause(struct vpt1_gw *gw, u8 subtype)
{
}
#endif

struct vpt1_dp {
	struct socket *sock;
	struct vpt1_gw *gw; /* Valid for the socket's life: unbound before destroy. */
	struct rcu_head rcu;
};

/* Inner packets ----------------------------------------------------------------- */

/* One synchronous AEAD operation over a single contiguous buffer: AAD|data|tag. */
static int aead_run(struct crypto_aead *tfm, u8 *buf, unsigned int total,
		    unsigned int cryptlen, u64 counter, bool encrypt)
{
	struct aead_request *req;
	struct scatterlist sg;
	u8 iv[VPT1_NONCE_LEN];
	int err;

	req = aead_request_alloc(tfm, GFP_ATOMIC);
	if (!req)
		return -ENOMEM;
	vpt1_counter_nonce(counter, iv);
	sg_init_one(&sg, buf, total);
	aead_request_set_callback(req, 0, NULL, NULL);
	aead_request_set_ad(req, VPT1_AAD_LEN);
	aead_request_set_crypt(req, &sg, &sg, cryptlen, iv);
	err = encrypt ? crypto_aead_encrypt(req) : crypto_aead_decrypt(req);
	/* A synchronous provider never returns -EINPROGRESS or -EBUSY. */
	if (WARN_ON_ONCE(err == -EINPROGRESS || err == -EBUSY))
		err = -EIO;
	aead_request_free(req);
	return err;
}

/* GSO bound ----------------------------------------------------------------------------- */

#define VPT1_XMIT_BATCH_MAX 64U

struct vpt1_xmit_run {
	const struct vpt1_gw *gw; /* The gateway whose list this CPU is in the middle of. */
	unsigned int count;
};

static DEFINE_PER_CPU(struct vpt1_xmit_run, vpt1_xmit_run);

/*
 * vpn0 advertises no GSO, so the stack segments an aggregate before ndo_start_xmit and hands the
 * segments over as one list: every segment but the last is sent with xmit_more set. Counting a
 * run of xmit_more transmissions therefore counts the segments of one aggregate (TECHNICAL_DESIGN
 * 9). The 65th and later are dropped and counted. Bottom halves are off in ndo_start_xmit. The
 * run is remembered with its gateway, so two vpn0 devices (two namespaces) on one CPU never share a
 * count.
 */
static bool xmit_batch_ok(const struct vpt1_gw *gw)
{
	struct vpt1_xmit_run *run = this_cpu_ptr(&vpt1_xmit_run);
	bool ok;

	/* One list goes to one device, so a run that belongs to another gateway is never this one's. */
	if (run->gw != gw) {
		run->gw = gw;
		run->count = 0;
	}
	ok = ++run->count <= VPT1_XMIT_BATCH_MAX;
	if (!netdev_xmit_more())
		run->count = 0;
	return ok;
}

/* Fragment jobs --------------------------------------------------------------------------- */

/*
 * One fragment on its way to the per-peer cache. Allocated in softirq, charged to the gateway's
 * shared fragment budget before it exists (a refusal costs nothing), freed by whichever of run()
 * or the cancelled call consumes it. It holds a reference on the peer (vpt1_job_queue) and on
 * the peer's continuity.
 */
struct vpt1_frag_job {
	struct vpt1_job job;
	struct vpt1_continuity *cont;
	struct vpt1_generation *gen; /* Tunnel side: the RX generation this fragment was opened under. */
	size_t charge;
	u16 length;
	u8 ingress;
	u8 data[] __counted_by(length);
};

static void frag_job_free(struct vpt1_gw *gw, struct vpt1_frag_job *fj)
{
	if (fj->gen)
		vpt1_generation_put(fj->gen);
	vpt1_continuity_put(fj->cont);
	vpt1_gw_frag_release(gw, fj->charge);
	kfree(fj);
}

static void frag_run(struct vpt1_job *job, bool cancelled);

/* Any context. @data is copied. The peer is referenced by the caller; counts every refusal. */
static void frag_queue(struct vpt1_gw *gw, struct vpt1_peer *peer, u8 ingress,
		       struct vpt1_generation *gen, const u8 *data, unsigned int length)
{
	size_t size = struct_size_t(struct vpt1_frag_job, data, length);
	size_t charge = kmalloc_size_roundup(size);
	struct vpt1_frag_job *fj;
	int err;

	if (!vpt1_peer_eligible(peer, ktime_get_boottime_ns())) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		return;
	}
	if (!vpt1_gw_frag_reserve(gw, charge)) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_BUSY);
		return;
	}
	fj = kmalloc(size, GFP_ATOMIC);
	if (!fj) {
		vpt1_gw_frag_release(gw, charge);
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_BUSY);
		return;
	}
	fj->cont = vpt1_peer_continuity_get(peer);
	if (!fj->cont) {
		vpt1_gw_frag_release(gw, charge);
		kfree(fj);
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		return;
	}
	fj->gen = gen;
	if (gen)
		vpt1_generation_get(gen);
	fj->charge = charge;
	fj->length = length;
	fj->ingress = ingress;
	fj->job.run = frag_run;
	memcpy(fj->data, data, length);
	err = vpt1_job_queue(peer, &fj->job);
	if (err) {
		frag_job_free(gw, fj);
		vpt1_gw_stat_inc(gw, err == -EBUSY ? VPT1_E_FRAG_BUSY : VPT1_E_FRAG_DROPPED);
		return;
	}
	vpt1_gw_frag_work_arm(gw);
	vpt1_gw_stat_inc(gw, VPT1_E_FRAG_QUEUED);
}

/* Transmit ---------------------------------------------------------------------- */

/*
 * Seal one inner IPv4 packet for @peer and hand it to the underlay. The caller holds
 * rcu_read_lock(), has bottom halves off and holds a reference on @peer; @data is borrowed. Policy
 * is the packet's @pi or, with @pi NULL, the ICMP error context @ectx. Returns true when a
 * datagram was handed to UDP output.
 */
static bool tx_seal_send(struct vpt1_gw *gw, struct vpt1_peer *peer, const u8 *data,
			 unsigned int iplen, const struct vpt1_pkt_info *pi,
			 const struct vpt1_error_ctx *ectx)
{
	struct vpt1_route route;
	struct vpt1_incarnation inc;
	struct vpt1_admit_ctx ctx;
	struct vpt1_generation *gen = NULL;
	struct sk_buff *out = NULL;
	struct rtable *rt;
	struct flowi4 fl4 = {};
	struct vpt1_dp *dp;
	enum vpt1_reason why;
	unsigned int total;
	bool in_flight = false, sent = false, reply = false;
	u64 counter, now = ktime_get_boottime_ns(), remote_rx = 0;
	unsigned int tries;
	u32 rule = 0;
	u8 *buf, tos = data[1] & 3; /* Outer DSCP best effort, normal ECN encapsulation. */
	int err;

	dp = vpt1_gw_priv(gw);
	if (!dp || vpt1_gw_peer_route(gw, vpt1_peer_slot(peer), &route)) {
		vpt1_gw_stat_inc(gw, VPT1_E_TX_NO_ROUTE);
		return false;
	}
	vpt1_peer_incarnation(peer, &inc);
	/*
	 * Processing admission: held context, packet policy, then handoff. A rekey can take
	 * over sending between getting the generation and admitting it; that one packet looks again
	 * rather than being lost.
	 */
	for (tries = 0;; tries++) {
		gen = vpt1_peer_tx_generation_get(peer, now, &remote_rx);
		if (!gen) {
			vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
			return false;
		}
		err = vpt1_admit_begin(peer, &inc, VPT1_DIR_TX, gen, now, &ctx, &why);
		if (err != -ENOKEY || tries)
			break;
		vpt1_generation_put(gen);
		gen = NULL;
	}
	if (!err)
		err = vpt1_admit_size(&ctx, iplen, &why);
	if (!err)
		err = pi ? vpt1_admit_packet(&ctx, pi, &why) : vpt1_admit_error(&ctx, ectx, &why);
	if (err) {
		vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
		if (!pi)
			vpt1_gw_stat_inc(gw, VPT1_E_ERROR_DENIED);
		goto out_unlock;
	}
	rule = ctx.rule_id;
	reply = ctx.reply;
	vpt1_admit_handoff(&ctx);
	in_flight = true;

	/* Processing is reserved in flight; seal before the final output recheck. */
	if (vpt1_generation_tx_reserve(gen, VPT1_PREFIX_LEN + iplen, now, &counter))
		goto fail;
	total = VPT1_AAD_LEN + VPT1_PREFIX_LEN + iplen + VPT1_TAG_LEN;
	out = alloc_skb(total + LL_MAX_HEADER + sizeof(struct iphdr) +
			sizeof(struct udphdr) + 16, GFP_ATOMIC);
	if (!out)
		goto fail;
	skb_reserve(out, LL_MAX_HEADER + sizeof(struct iphdr) + sizeof(struct udphdr) + 16);
	buf = skb_put(out, total);
	memcpy(buf, "VPT1", 4);
	buf[4] = VPT1_RECORD;
	buf[5] = 0;
	put_unaligned_be16(total - VPT1_ENVELOPE_LEN, buf + 6);
	put_unaligned_be64(remote_rx, buf + 8);
	put_unaligned_be64(counter, buf + VPT1_ENVELOPE_LEN);
	buf[VPT1_AAD_LEN] = VPT1_IPV4;
	buf[VPT1_AAD_LEN + 1] = 0;
	put_unaligned_be16(iplen, buf + VPT1_AAD_LEN + 2);
	memcpy(buf + VPT1_AAD_LEN + VPT1_PREFIX_LEN, data, iplen);
	if (aead_run(vpt1_generation_tx_aead(gen), buf, total, VPT1_PREFIX_LEN + iplen,
		     counter, true))
		goto fail;

	fl4.daddr = route.endpoint;
	fl4.saddr = route.local;
	fl4.flowi4_oif = route.ifindex;
	fl4.flowi4_proto = IPPROTO_UDP;
	rt = ip_route_output_key(vpt1_gw_net(gw), &fl4);
	if (IS_ERR(rt))
		goto fail;
	tx_test_pause();
	/* Crypto and routing ran without the peer lock. Tightening, revoke, lease
	 * expiry or TX promotion during that work must not emit stale ciphertext.
	 * Reserved counters are never refunded, even when this final check fails.
	 */
	if (pi ? vpt1_admit_tx_recheck(peer, &inc, gen, pi, iplen, ktime_get_boottime_ns(), &why) :
		 vpt1_admit_tx_recheck_error(peer, &inc, gen, ectx, iplen, ktime_get_boottime_ns(),
					     &why)) {
		ip_rt_put(rt);
		goto fail;
	}
	udp_tunnel_xmit_skb(rt, dp->sock->sk, out, fl4.saddr, fl4.daddr, tos,
			    ip4_dst_hoplimit(&rt->dst), htons(IP_DF),
			    htons(VPT1_UDP_PORT), route.port, false, false, 0);
	out = NULL;
	sent = true;
	vpt1_gw_stat_inc(gw, VPT1_E_TX_PACKETS);
	if (!pi)
		vpt1_gw_stat_inc(gw, VPT1_E_ERROR_ADMITTED);
	else if (!(get_unaligned_be16(data + 6) & IP_OFFSET))
		/* A later fragment cannot be the subject of an ICMP error. */
		vpt1_peer_err_save(peer, VPT1_DIR_TX, rule, reply, data, iplen, now);
	goto out_unlock;
fail:
	vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
out_unlock:
	if (in_flight)
		vpt1_admit_done(peer);
	if (gen)
		vpt1_generation_put(gen);
	if (out)
		kfree_skb(out);
	return sent;
}

/* The peer owning @dst, referenced, or NULL (counted). RCU read side. */
static struct vpt1_peer *tx_peer(struct vpt1_gw *gw, __be32 dst)
{
	struct vpt1_pkt_info key = { .dst = dst };
	struct vpt1_route route;
	struct vpt1_peer *peer;

	if (!vpt1_gw_priv(gw) || vpt1_gw_tx_route(gw, &key, &route)) {
		vpt1_gw_stat_inc(gw, VPT1_E_TX_NO_ROUTE);
		return NULL;
	}
	peer = vpt1_gw_peer_get(gw, route.slot);
	if (!peer)
		vpt1_gw_stat_inc(gw, VPT1_E_TX_NO_ROUTE);
	return peer;
}

/* LAN to tunnel: one fragment, checked on its own and queued for peer-scoped normalization. */
static void tx_frag_intake(struct vpt1_gw *gw, const u8 *data, unsigned int iplen)
{
	struct vpt1_peer *peer;
	__be32 src, dst;

	if (vpt1_ip_fragment_check(data, iplen)) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		return;
	}
	memcpy(&src, data + 12, sizeof(src));
	memcpy(&dst, data + 16, sizeof(dst));
	rcu_read_lock();
	peer = tx_peer(gw, dst);
	if (peer) {
		/* A fragment has no ports: only source and destination ownership identify it. */
		if (!vpt1_gw_lan_has(gw, src, false) || iplen > vpt1_peer_cap(peer))
			vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		else
			frag_queue(gw, peer, VPT1_FRAG_LOCAL, NULL, data, iplen);
		vpt1_peer_put(peer);
	}
	rcu_read_unlock();
}

/* An ICMP error toward the peer: only one that answers a packet recently received from it. */
static bool tx_error(struct vpt1_gw *gw, const u8 *data, unsigned int iplen)
{
	struct vpt1_error_ctx ectx;
	struct vpt1_pkt_info pi;
	struct vpt1_peer *peer;
	bool sent = false;

	if (vpt1_ip_error_validate(data, iplen, &pi)) {
		vpt1_gw_stat_inc(gw, VPT1_E_ERROR_DENIED);
		return false;
	}
	rcu_read_lock();
	peer = tx_peer(gw, pi.dst);
	if (peer) {
		/* The sender is a LAN host or this gateway's own LAN address. */
		if (!vpt1_gw_lan_has(gw, pi.src, true) ||
		    vpt1_peer_err_find(peer, VPT1_DIR_RX, data, iplen, ktime_get_boottime_ns(),
				       &ectx)) {
			vpt1_gw_stat_inc(gw, VPT1_E_ERROR_DENIED);
			vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
		} else {
			sent = tx_seal_send(gw, peer, data, iplen, NULL, &ectx);
		}
		vpt1_peer_put(peer);
	}
	rcu_read_unlock();
	return sent;
}

/* Consumes skb. Returns true when a datagram was handed to the underlay. */
bool vpt1_dp_xmit(struct vpt1_gw *gw, struct net_device *dev, struct sk_buff *skb)
{
	struct vpt1_pkt_info pi;
	struct vpt1_peer *peer;
	bool sent = false;
	const u8 *data;
	unsigned int iplen;

	if (!xmit_batch_ok(gw)) {
		vpt1_gw_stat_inc(gw, VPT1_E_TX_GSO_LIMIT);
		vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
		goto out_free;
	}
	if (vpt1_ip_skb_linear(skb) || skb->len > VPT1_INNER_CAP_MAX) {
		vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
		goto out_free;
	}
	data = skb->data;
	iplen = skb->len;
	if (vpt1_ip_is_fragment(data, iplen)) {
		/* Queued, never sent from here: the set completes (or not) in the worker. */
		tx_frag_intake(gw, data, iplen);
		goto out_free;
	}
	if (vpt1_ip_is_icmp_error(data, iplen)) {
		sent = tx_error(gw, data, iplen);
		goto out_free;
	}
	if (vpt1_ip_validate(data, iplen, &pi)) {
		vpt1_gw_stat_inc(gw, VPT1_E_TX_DROPPED);
		goto out_free;
	}
	/* Everything below is non-sleeping and stays inside one RCU read section. */
	rcu_read_lock();
	peer = tx_peer(gw, pi.dst);
	if (peer) {
		sent = tx_seal_send(gw, peer, data, iplen, &pi, NULL);
		vpt1_peer_put(peer);
	}
	rcu_read_unlock();
out_free:
	consume_skb(skb);
	return sent;
}

/*
 * Seal and send one encrypted control record under the peer's current generation, or its candidate
 * (CONFIRM, READY). It takes a counter of that generation like data does, so it counts toward the
 * key's limits, but it is not an inner packet: there is no policy or flow check, and it never
 * reaches vpn0. A lapsed lease, a stopping gateway or a closed peer sends nothing. Any context.
 * Under the current generation the record also takes a processing reservation and passes the
 * shared final cut after crypto and routing, so a revoke or lease lapse during that work wins. A
 * candidate is not yet the peer's session and is not admitted: only the lease is checked again.
 */
static int vpt1_dp_send_control(struct vpt1_gw *gw, struct vpt1_peer *peer, enum vpt1_send_gen which,
				u8 subtype,
				const u8 *payload, unsigned int len)
{
	struct vpt1_generation *gen;
	struct vpt1_incarnation inc;
	struct vpt1_admit_ctx ctx;
	struct vpt1_route route;
	struct flowi4 fl4 = {};
	struct sk_buff *out;
	struct rtable *rt;
	struct vpt1_dp *dp;
	u64 counter, now, remote_rx;
	unsigned int total;
	int err = -ENOTCONN;
	enum vpt1_reason why;
	bool in_flight = false, cut = which == VPT1_SEND_CURRENT;
	u8 *buf;

	if (!payload || ((subtype == VPT1_PING || subtype == VPT1_PONG) && len != VPT1_CHALLENGE_LEN))
		return -EINVAL;
	now = ktime_get_boottime_ns();
	local_bh_disable();
	rcu_read_lock();
	dp = vpt1_gw_priv(gw);
	if (!dp || !vpt1_gw_lease_live(gw, now) ||
	    vpt1_gw_peer_route(gw, vpt1_peer_slot(peer), &route))
		goto unlock;
	gen = which == VPT1_SEND_CANDIDATE ? vpt1_peer_candidate_tx_get(peer, &remote_rx) :
	      vpt1_peer_tx_generation_get(peer, now, &remote_rx);
	if (!gen) {
		err = -ENOKEY;
		goto unlock;
	}
	vpt1_peer_incarnation(peer, &inc);
	if (cut) {
		err = vpt1_admit_control_begin(peer, &inc, gen, now, &ctx, &why);
		if (err)
			goto put;
		vpt1_admit_handoff(&ctx);
		in_flight = true;
	}
	err = vpt1_generation_tx_reserve(gen, VPT1_PREFIX_LEN + len, now, &counter);
	if (err)
		goto put;
	total = VPT1_AAD_LEN + VPT1_PREFIX_LEN + len + VPT1_TAG_LEN;
	out = alloc_skb(total + LL_MAX_HEADER + sizeof(struct iphdr) + sizeof(struct udphdr) + 16,
			GFP_ATOMIC);
	if (!out) {
		err = -ENOMEM;
		goto put;
	}
	skb_reserve(out, LL_MAX_HEADER + sizeof(struct iphdr) + sizeof(struct udphdr) + 16);
	buf = skb_put(out, total);
	memcpy(buf, "VPT1", 4);
	buf[4] = VPT1_RECORD;
	buf[5] = 0;
	put_unaligned_be16(total - VPT1_ENVELOPE_LEN, buf + 6);
	put_unaligned_be64(remote_rx, buf + 8);
	put_unaligned_be64(counter, buf + VPT1_ENVELOPE_LEN);
	buf[VPT1_AAD_LEN] = subtype;
	buf[VPT1_AAD_LEN + 1] = 0;
	put_unaligned_be16(len, buf + VPT1_AAD_LEN + 2);
	memcpy(buf + VPT1_AAD_LEN + VPT1_PREFIX_LEN, payload, len);
	err = aead_run(vpt1_generation_tx_aead(gen), buf, total, VPT1_PREFIX_LEN + len, counter, true);
	if (err)
		goto free;
	fl4.daddr = route.endpoint;
	fl4.saddr = route.local;
	fl4.flowi4_oif = route.ifindex;
	fl4.flowi4_proto = IPPROTO_UDP;
	rt = ip_route_output_key(vpt1_gw_net(gw), &fl4);
	if (IS_ERR(rt)) {
		err = PTR_ERR(rt);
		goto free;
	}
	/* A receive-side PONG runs without cfg_lock; a revoke, lease deadline or
	 * generation promotion during crypto must win at the shared final cut.
	 */
	control_test_pause(gw, subtype);
	err = cut ? vpt1_admit_control_recheck(peer, &inc, gen, ktime_get_boottime_ns(), &why) :
		    (vpt1_gw_lease_live(gw, ktime_get_boottime_ns()) ? 0 : -ESTALE);
	if (err) {
		ip_rt_put(rt);
		goto free;
	}
	udp_tunnel_xmit_skb(rt, dp->sock->sk, out, fl4.saddr, fl4.daddr, 0,
			    ip4_dst_hoplimit(&rt->dst), htons(IP_DF), htons(VPT1_UDP_PORT),
			    route.port, false, false, 0);
	out = NULL;
	if (subtype == VPT1_PING)
		vpt1_gw_stat_inc(gw, VPT1_E_TX_PING);
	else if (subtype == VPT1_PONG)
		vpt1_gw_stat_inc(gw, VPT1_E_TX_PONG);
	else
		vpt1_gw_stat_inc(gw, VPT1_E_TX_ACTIVATION);
	err = 0;
free:
	if (out)
		kfree_skb(out);
put:
	if (in_flight)
		vpt1_admit_done(peer);
	vpt1_generation_put(gen);
unlock:
	rcu_read_unlock();
	local_bh_enable();
	return err;
}

/* Tell the owning controller that a grant arrived or a re-key is needed. Any context. */
static void vpt1_dp_event(struct vpt1_gw *gw, struct vpt1_peer *peer, enum vpt1_ctl_event event,
			  const u8 *attempt)
{
	struct vpt1_incarnation inc;

	vpt1_peer_incarnation(peer, &inc);
	vpt1_netlink_rekey_event(vpt1_gw_net(gw), gw, vpt1_peer_slot(peer), &inc, event, attempt);
}

/* Process context, cfg_lock held: send one handshake message from the bound socket. */
int vpt1_dp_send_exchange(struct vpt1_gw *gw, unsigned int slot, const void *buf, size_t len)
{
	struct vpt1_route route;
	struct vpt1_dp *dp;
	struct sockaddr_in to = { .sin_family = AF_INET };
	struct msghdr msg = { .msg_name = &to, .msg_namelen = sizeof(to) };
	struct kvec iov = { .iov_base = (void *)buf, .iov_len = len };
	int err;

	rcu_read_lock();
	dp = vpt1_gw_priv(gw);
	err = dp ? vpt1_gw_peer_route(gw, slot, &route) : -ENOTCONN;
	if (!err) {
		to.sin_addr.s_addr = route.endpoint;
		to.sin_port = route.port;
	}
	rcu_read_unlock();
	if (err)
		return err;
	/* The socket cannot go away while the caller holds cfg_lock (unbind needs it). */
	err = kernel_sendmsg(dp->sock, &msg, &iov, 1, len);
	if (err < 0)
		return err;
	vpt1_gw_stat_inc(gw, VPT1_E_TX_EXCHANGE);
	return err == (int)len ? 0 : -EMSGSIZE;
}

/* Receive ----------------------------------------------------------------------- */

/* Drop helper: counts, frees, and tells the UDP layer the skb was consumed. */
static int rx_drop(struct vpt1_gw *gw, struct sk_buff *skb, unsigned int counter)
{
	vpt1_gw_stat_inc(gw, counter);
	kfree_skb(skb);
	return 0;
}

/*
 * INIT and RESPONSE are unauthenticated here: they are cleartext Noise messages that
 * only the controller can verify. The kernel's job is to pass along those that come from
 * a configured endpoint, rate-limited, to the owner, and nothing else.
 */
static int rx_exchange(struct vpt1_gw *gw, struct sk_buff *skb, __be32 saddr, __be16 sport,
		       const u8 *init_attempt)
{
	struct vpt1_incarnation inc;
	struct vpt1_peer *peer;
	unsigned int slot;
	int err;

	rcu_read_lock();
	peer = vpt1_gw_peer_by_endpoint(gw, saddr, sport);
	rcu_read_unlock();
	if (!peer)
		return rx_drop(gw, skb, VPT1_E_RX_BAD_SOURCE);
	if (!vpt1_peer_exchange_admit(peer, ktime_get_boottime_ns())) {
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_EXCHANGE_DROPPED);
	}
	vpt1_peer_incarnation(peer, &inc);
	slot = vpt1_peer_slot(peer);
	/* The attempt a responder may begin: the last INIT handed to the owner, nothing the owner invents. */
	if (init_attempt)
		vpt1_peer_note_init(peer, init_attempt, ktime_get_boottime_ns());
	vpt1_peer_put(peer);
	err = vpt1_netlink_exchange_rx(vpt1_gw_net(gw), gw, slot, &inc, skb->data, skb->len);
	if (err)
		return rx_drop(gw, skb, VPT1_E_RX_EXCHANGE_DROPPED);
	vpt1_gw_stat_inc(gw, VPT1_E_RX_EXCHANGE);
	kfree_skb(skb);
	return 0;
}

/*
 * A tunnel-side fragment, authenticated and counted. It is source-checked against the
 * authenticated peer (no later stage sees the ports of a non-first fragment), its destination
 * must be a LAN host, the outer ECN mark is merged into it, and it is queued. Consumes skb and
 * the gen/peer references.
 */
static int rx_frag(struct vpt1_gw *gw, struct vpt1_peer *peer, struct vpt1_generation *gen,
		   struct sk_buff *skb, unsigned int length, u8 outer_tos)
{
	u8 *frag = skb->data + VPT1_AAD_LEN + VPT1_PREFIX_LEN;
	__be32 src, dst;
	bool ok;
	int err;

	err = vpt1_ip_frag_ecn_apply(frag, length, outer_tos);
	if (err > 0)
		vpt1_gw_stat_inc(gw, VPT1_E_RX_ECN_ANOMALY);
	memcpy(&src, frag + 12, sizeof(src));
	memcpy(&dst, frag + 16, sizeof(dst));
	rcu_read_lock();
	ok = err >= 0 && vpt1_peer_owns(peer, src) && vpt1_gw_lan_has(gw, dst, false) &&
	     length <= vpt1_peer_cap(peer);
	rcu_read_unlock();
	if (!ok)
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
	else
		frag_queue(gw, peer, VPT1_FRAG_TUNNEL, gen, frag, length);
	vpt1_generation_put(gen);
	vpt1_peer_put(peer);
	kfree_skb(skb);
	return 0;
}

static int vpt1_dp_rcv(struct sock *sk, struct sk_buff *skb)
{
	struct vpt1_dp *dp = rcu_dereference_sk_user_data(sk);
	struct vpt1_datagram view;
	struct vpt1_plaintext plain;
	struct vpt1_pkt_info pi;
	struct vpt1_error_ctx ectx;
	struct vpt1_incarnation inc;
	struct vpt1_admit_ctx ctx;
	struct vpt1_generation *gen;
	struct net_device *dev;
	struct vpt1_peer *peer;
	enum vpt1_reason why;
	struct vpt1_gw *gw;
	unsigned int len, ptlen;
	__be32 saddr;
	__be16 sport;
	bool is_error;
	u8 outer_tos;
	u64 now;
	int err;

	if (!dp)
		goto drop_plain;
	gw = dp->gw;
	/* Only the outer envelope is trusted for nothing: check it and decide. */
	if (!pskb_may_pull(skb, sizeof(struct udphdr) + VPT1_ENVELOPE_LEN))
		return rx_drop(gw, skb, VPT1_E_RX_MALFORMED);
	saddr = ip_hdr(skb)->saddr;
	outer_tos = ip_hdr(skb)->tos;
	sport = udp_hdr(skb)->source;
	/* UDP checksums are required in both directions. */
	if (!udp_hdr(skb)->check || (!skb_csum_unnecessary(skb) && __skb_checksum_complete(skb)))
		return rx_drop(gw, skb, VPT1_E_RX_MALFORMED);
	len = ntohs(udp_hdr(skb)->len);
	if (len < sizeof(struct udphdr) || len > skb->len ||
	    len - sizeof(struct udphdr) > VPT1_INNER_CAP_MAX + VPT1_RECORD_EXPANSION)
		return rx_drop(gw, skb, VPT1_E_RX_MALFORMED);
	skb_pull(skb, sizeof(struct udphdr));
	len -= sizeof(struct udphdr);
	if (skb->len > len && pskb_trim(skb, len))
		return rx_drop(gw, skb, VPT1_E_RX_MALFORMED);
	/* Decrypt in place: a linear, privately owned copy of the datagram. */
	if (skb_linearize(skb) || skb_ensure_writable(skb, skb->len))
		return rx_drop(gw, skb, VPT1_E_RX_DROPPED);
	if (vpt1_parse_datagram(skb->data, skb->len, VPT1_INNER_CAP_MAX, &view))
		return rx_drop(gw, skb, VPT1_E_RX_MALFORMED);
	if (view.kind != VPT1_RECORD)
		return rx_exchange(gw, skb, saddr, sport, view.kind == VPT1_INIT ? view.attempt : NULL);

	rcu_read_lock();
	peer = vpt1_gw_peer_by_locator(gw, view.locator);
	if (!peer) {
		rcu_read_unlock();
		return rx_drop(gw, skb, VPT1_E_RX_UNKNOWN_LOCATOR);
	}
	/* The outer source must be the configured endpoint; a locator is only a hint. */
	if (!vpt1_peer_source_ok(peer, saddr, sport)) {
		rcu_read_unlock();
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_BAD_SOURCE);
	}
	now = ktime_get_boottime_ns();
	/* The generation this record is addressed to: current, next or (within grace) prev. */
	gen = vpt1_peer_rx_generation_get(peer, view.locator, now);
	rcu_read_unlock();
	if (!gen) {
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_DROPPED);
	}

	/* Read-only replay precheck, then authenticate, then claim the counter. */
	err = vpt1_generation_rx_precheck(gen, view.counter, now);
	if (err) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb,
			       err == -EALREADY || err == -ESTALE ? VPT1_E_RX_REPLAY
								    : VPT1_E_RX_DROPPED);
	}
	err = aead_run(vpt1_generation_rx_aead(gen), skb->data, skb->len,
		       skb->len - VPT1_AAD_LEN, view.counter, false);
	if (err) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_AUTH_FAIL);
	}
	ptlen = view.ciphertext_len;
	now = ktime_get_boottime_ns();
	err = vpt1_generation_rx_accept(gen, view.counter, ptlen, now);
	if (err) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb,
			       err == -EALREADY || err == -ESTALE ? VPT1_E_RX_REPLAY
								    : VPT1_E_RX_DROPPED);
	}

	/* Authenticated: an encrypted control record, or an IPv4 data record for the policy path. */
	if (!vpt1_parse_authenticated_plaintext(skb->data + VPT1_AAD_LEN, ptlen, VPT1_INNER_CAP_MAX,
						&plain) &&
	    plain.subtype != VPT1_IPV4) {
		/*
		 * Control. PING and PONG speak for the session only under the current generation; the
		 * activation and re-key records are checked against their expected generation, role,
		 * state and correlation value by vpt1_peer_control_rx(). Nothing is acted on while the
		 * lease is lapsed, and nothing here ever reaches vpn0.
		 */
		if (!vpt1_gw_lease_live(gw, now)) {
			vpt1_gw_stat_inc(gw, VPT1_E_RX_CONTROL_DROPPED);
		} else if (plain.subtype == VPT1_PING || plain.subtype == VPT1_PONG) {
			if (!vpt1_peer_gen_is_current(peer, gen)) {
				vpt1_gw_stat_inc(gw, VPT1_E_RX_CONTROL_DROPPED);
			} else if (plain.subtype == VPT1_PING) {
				vpt1_gw_stat_inc(gw, VPT1_E_RX_PING);
				vpt1_dp_send_control(gw, peer, VPT1_SEND_CURRENT, VPT1_PONG,
						     plain.payload, plain.payload_len);
			} else if (vpt1_peer_live_pong(peer, plain.payload, now)) {
				vpt1_gw_stat_inc(gw, VPT1_E_RX_PONG);
			} else {
				vpt1_gw_stat_inc(gw, VPT1_E_RX_CONTROL_DROPPED);
			}
		} else {
			struct vpt1_ctl_out reply;

			if (vpt1_peer_control_rx(peer, gen, plain.subtype, plain.payload,
						 plain.payload_len, now, &reply)) {
				vpt1_gw_stat_inc(gw, VPT1_E_RX_CONTROL_DROPPED);
			} else {
				if (reply.subtype)
					vpt1_dp_send_control(gw, peer, reply.which, reply.subtype,
							     reply.payload, reply.len);
				if (reply.event != VPT1_EVT_NONE)
					vpt1_dp_event(gw, peer, reply.event, reply.attempt);
			}
		}
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		kfree_skb(skb);
		return 0;
	}
	/* Application data needs a committed generation: the candidate carries control only. */
	if (vpt1_peer_gen_is_candidate(peer, gen)) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_CANDIDATE_DROPPED);
	}
	if (vpt1_parse_authenticated_plaintext(skb->data + VPT1_AAD_LEN, ptlen,
					       VPT1_INNER_CAP_MAX, &plain) ||
	    plain.subtype != VPT1_IPV4) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_DROPPED);
	}
	/* A fragment is normalized before anything reads its transport header. */
	if (vpt1_ip_is_fragment(plain.payload, plain.payload_len))
		return rx_frag(gw, peer, gen, skb, plain.payload_len, outer_tos);
	is_error = vpt1_ip_is_icmp_error(plain.payload, plain.payload_len);
	err = is_error ? vpt1_ip_error_validate(plain.payload, plain.payload_len, &pi) :
			 vpt1_ip_validate(plain.payload, plain.payload_len, &pi);
	if (err) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, is_error ? VPT1_E_ERROR_DENIED : VPT1_E_RX_DROPPED);
	}

	err = vpt1_ip_ecn_apply(skb->data + VPT1_AAD_LEN + VPT1_PREFIX_LEN,
				plain.payload_len, outer_tos);
	if (err)
		vpt1_gw_stat_inc(gw, VPT1_E_RX_ECN_ANOMALY);
	if (err < 0) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, VPT1_E_RX_DROPPED);
	}
	if (is_error) {
		/*
		 * An error from the peer's site (a host or its gateway's LAN address) to a LAN
		 * host, and only for a packet we sent that host's way recently; the saved rule
		 * is checked again below.
		 */
		rcu_read_lock();
		err = vpt1_peer_owns(peer, pi.src) && vpt1_gw_lan_has(gw, pi.dst, false) ?
		      vpt1_peer_err_find(peer, VPT1_DIR_TX, plain.payload, plain.payload_len,
					 ktime_get_boottime_ns(), &ectx) : -EACCES;
		rcu_read_unlock();
		if (err) {
			vpt1_generation_put(gen);
			vpt1_peer_put(peer);
			return rx_drop(gw, skb, VPT1_E_ERROR_DENIED);
		}
	}

	/* Final admission, then hand the inner packet to the stack on vpn0. */
	rx_test_pause();
	now = ktime_get_boottime_ns();
	vpt1_peer_incarnation(peer, &inc);
	rcu_read_lock();
	err = vpt1_admit_begin(peer, &inc, VPT1_DIR_RX, gen, now, &ctx, &why);
	if (!err)
		err = vpt1_admit_size(&ctx, plain.payload_len, &why);
	if (!err)
		err = is_error ? vpt1_admit_error(&ctx, &ectx, &why) :
				 vpt1_admit_packet(&ctx, &pi, &why);
	if (!err) {
		dev = dev_get_by_index_rcu(vpt1_gw_net(gw), vpt1_gw_ifindex(gw));
		if (!dev) {
			vpt1_admit_abort(&ctx);
			err = -ENODEV;
		}
	}
	if (!err)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	if (err) {
		vpt1_generation_put(gen);
		vpt1_peer_put(peer);
		return rx_drop(gw, skb, is_error ? VPT1_E_ERROR_DENIED : VPT1_E_RX_POLICY);
	}
	if (is_error)
		vpt1_gw_stat_inc(gw, VPT1_E_ERROR_ADMITTED);
	else
		vpt1_peer_err_save(peer, VPT1_DIR_RX, ctx.rule_id, ctx.reply,
				   skb->data + VPT1_AAD_LEN + VPT1_PREFIX_LEN,
				   plain.payload_len, now);

	skb_pull(skb, VPT1_AAD_LEN + VPT1_PREFIX_LEN);
	skb_trim(skb, plain.payload_len);
	skb_reset_network_header(skb);
	skb->protocol = htons(ETH_P_IP);
	skb->pkt_type = PACKET_HOST;
	/* Clear marks, conntrack state and the outer dst: the inner packet is new here. */
	skb_scrub_packet(skb, true);
	skb->dev = dev;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->csum_level = 0;
	vpt1_gw_stat_inc(gw, VPT1_E_RX_PACKETS);
	vpt1_dev_rx_add(dev, skb->len);
	netif_rx(skb);
	vpt1_admit_done(peer);
	vpt1_generation_put(gen);
	vpt1_peer_put(peer);
	return 0;
drop_plain:
	kfree_skb(skb);
	return 0;
}

/* Fragment job processing (gateway worker, process context) ---------------------------------- */

/* A normalized LAN datagram: sent as ordinary records, cut to the pair cap when it is larger. */
static void frag_tx_complete(struct vpt1_gw *gw, struct vpt1_peer *peer, const u8 *packet,
			     unsigned int length, const struct vpt1_pkt_info *pi)
{
	unsigned int cap, count, i;
	bool all = true;
	u8 *buf;
	int n;

	rcu_read_lock();
	cap = vpt1_peer_cap(peer);
	rcu_read_unlock();
	count = cap ? vpt1_ip_fragment_count(length, cap) : 0;
	buf = count ? kmalloc(cap, GFP_KERNEL) : NULL;
	if (!buf) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		return;
	}
	for (i = 0; i < count && all; i++) {
		n = vpt1_ip_fragment_build(packet, length, cap, i, buf);
		if (n < 0) {
			all = false;
			break;
		}
		/* Each output fragment is its own record; policy is the datagram's. */
		local_bh_disable();
		rcu_read_lock();
		all = tx_seal_send(gw, peer, buf, n, pi, NULL);
		rcu_read_unlock();
		local_bh_enable();
	}
	kfree(buf);
	vpt1_gw_stat_inc(gw, all ? VPT1_E_FRAG_COMPLETE : VPT1_E_FRAG_DROPPED);
}

/* A normalized tunnel datagram: final admission against CURRENT policy, then one delivery. */
static void frag_rx_complete(struct vpt1_gw *gw, struct vpt1_peer *peer,
			     struct vpt1_generation *gen, const u8 *packet, unsigned int length,
			     const struct vpt1_pkt_info *pi)
{
	struct vpt1_incarnation inc;
	struct vpt1_admit_ctx ctx;
	struct net_device *dev = NULL;
	struct sk_buff *skb;
	enum vpt1_reason why;
	u64 now;
	int err;

	/*
	 * @gen is the RX generation the fragment that completed the set was opened under (a candidate
	 * never reaches here). Admitting under it applies the limits, grace and liveness of the key that
	 * actually carried the datagram: after a re-key the current TX key may be another one, and a
	 * fragment under a generation that has since left its grace is refused, not delivered.
	 */
	skb = gen ? alloc_skb(length + LL_MAX_HEADER, GFP_KERNEL) : NULL;
	if (!skb) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		return;
	}
	skb_reserve(skb, LL_MAX_HEADER);
	skb_put_data(skb, packet, length);
	vpt1_peer_incarnation(peer, &inc);
	rx_test_pause();
	now = ktime_get_boottime_ns();
	rcu_read_lock();
	/* Each fragment fitted the pair cap on arrival; the whole datagram is larger by design. */
	err = vpt1_admit_begin(peer, &inc, VPT1_DIR_RX, gen, now, &ctx, &why);
	if (!err)
		err = vpt1_admit_packet(&ctx, pi, &why);
	if (!err) {
		dev = dev_get_by_index_rcu(vpt1_gw_net(gw), vpt1_gw_ifindex(gw));
		if (!dev) {
			vpt1_admit_abort(&ctx);
			err = -ENODEV;
		}
	}
	if (!err)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	if (err) {
		vpt1_gw_stat_inc(gw, VPT1_E_RX_POLICY);
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		kfree_skb(skb);
		return;
	}
	vpt1_peer_err_save(peer, VPT1_DIR_RX, ctx.rule_id, ctx.reply, packet, length, now);
	skb_reset_network_header(skb);
	skb->protocol = htons(ETH_P_IP);
	skb->pkt_type = PACKET_HOST;
	skb_scrub_packet(skb, true);
	skb->dev = dev;
	skb->ip_summed = CHECKSUM_UNNECESSARY; /* The whole datagram's checksums were verified. */
	skb->csum_level = 0;
	vpt1_gw_stat_inc(gw, VPT1_E_RX_PACKETS);
	vpt1_gw_stat_inc(gw, VPT1_E_FRAG_COMPLETE);
	vpt1_dev_rx_add(dev, skb->len);
	netif_rx(skb);
	vpt1_admit_done(peer);
}

/*
 * Submit one fragment to the peer's cache. The job's input is consumed by the submission (the cache
 * copies it). A completed set is complete only under a live continuity, and gets the full transport
 * and checksum validation before any policy is looked at.
 */
static void frag_process(struct vpt1_gw *gw, struct vpt1_peer *peer, struct vpt1_frag_job *fj)
{
	struct vpt1_frag_scope scope = { .context = fj->cont, .ingress = fj->ingress };
	struct vpt1_frag_result *result = NULL;
	struct vpt1_frag_cache *cache;
	struct vpt1_generation *gen = fj->gen;
	struct vpt1_frag_view view;
	struct vpt1_pkt_info pi;
	int err;

	fj->gen = NULL; /* This call owns the reference from here: the completion may need it. */
	cache = vpt1_peer_frag_cache(peer);
	if (IS_ERR(cache)) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		frag_job_free(gw, fj);
		if (gen)
			vpt1_generation_put(gen);
		return;
	}
	err = vpt1_frag_submit(cache, &scope, fj->data, fj->length, ktime_get_boottime_ns(),
			       &result);
	frag_job_free(gw, fj);
	if (err <= 0) {
		/* Held (0): the periodic expiry or a later fragment decides. */
		if (err < 0)
			vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		if (gen)
			vpt1_generation_put(gen);
		return;
	}
	vpt1_frag_view(result, &view);
	/* Complete only under a context that is still the peer's session, and DF clear. */
	if (vpt1_continuity_dead(view.context) || view.frag_pmtu ||
	    vpt1_ip_validate(view.packet, view.length, &pi)) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
	} else if (view.ingress == VPT1_FRAG_LOCAL) {
		frag_tx_complete(gw, peer, view.packet, view.length, &pi);
	} else {
		frag_rx_complete(gw, peer, gen, view.packet, view.length, &pi);
	}
	if (gen)
		vpt1_generation_put(gen);
	vpt1_frag_result_free(result);
}

/* Gateway worker. The cancelled call may hold cfg_lock: it only frees what the job owns. */
static void frag_run(struct vpt1_job *job, bool cancelled)
{
	struct vpt1_frag_job *fj = container_of(job, struct vpt1_frag_job, job);
	struct vpt1_peer *peer = job->peer;
	struct vpt1_gw *gw = vpt1_peer_gw(peer);

	if (cancelled) {
		vpt1_gw_stat_inc(gw, VPT1_E_FRAG_DROPPED);
		frag_job_free(gw, fj);
		return;
	}
	frag_process(gw, peer, fj);
}

/* Socket binding ------------------------------------------------------------------ */

static void dp_free(struct rcu_head *rcu)
{
	kfree(container_of(rcu, struct vpt1_dp, rcu));
}

/*
 * The device has carrier exactly while an outer socket is bound: without carrier Linux
 * keeps the no-op qdisc and drops everything before vpn0's transmit function runs.
 */
static void dp_carrier(struct vpt1_gw *gw, bool on)
{
	struct net_device *dev;

	rcu_read_lock();
	dev = dev_get_by_index_rcu(vpt1_gw_net(gw), vpt1_gw_ifindex(gw));
	if (dev) {
		if (on)
			netif_carrier_on(dev);
		else
			netif_carrier_off(dev);
	}
	rcu_read_unlock();
}

static int vpt1_dp_bind(struct vpt1_gw *gw, __be32 local, u32 ifindex)
{
	struct udp_tunnel_sock_cfg tcfg = { .encap_type = 1, .encap_rcv = vpt1_dp_rcv };
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = local,
				    .sin_port = htons(VPT1_UDP_PORT) };
	struct net *net = vpt1_gw_net(gw);
	struct vpt1_dp *dp;
	int err;

	dp = kzalloc(sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	dp->gw = gw;
	err = sock_create_kern(net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &dp->sock);
	if (err)
		goto free;
	/* Bound to the gateway's outer address and underlay interface, nothing else. */
	err = sock_bindtoindex(dp->sock->sk, ifindex, true);
	if (!err)
		err = kernel_bind(dp->sock, (struct sockaddr *)&addr, sizeof(addr));
	if (err) {
		sock_release(dp->sock);
		goto free;
	}
	dp->sock->sk->sk_no_check_tx = 0; /* Send real UDP checksums. */
	tcfg.sk_user_data = dp;
	setup_udp_tunnel_sock(net, dp->sock, &tcfg);
	vpt1_gw_set_priv(gw, dp);
	dp_carrier(gw, true);
	return 0;
free:
	kfree(dp);
	return err;
}

static void vpt1_dp_unbind(struct vpt1_gw *gw)
{
	struct vpt1_dp *dp = vpt1_gw_priv(gw);

	if (!dp)
		return;
	dp_carrier(gw, false);
	vpt1_gw_set_priv(gw, NULL);
	udp_tunnel_sock_release(dp->sock);
	/* Callbacks and transmit paths that already hold dp finish under RCU. */
	call_rcu(&dp->rcu, dp_free);
}

const struct vpt1_gw_ops vpt1_dp_ops = {
	.send_control = vpt1_dp_send_control,
	.control_event = vpt1_dp_event,
	.bind = vpt1_dp_bind,
	.unbind = vpt1_dp_unbind,
	.set_mtu = vpt1_dev_set_mtu,
};
