// SPDX-License-Identifier: GPL-2.0-only
/* AY1 lifecycle foundation. No packet can be authorized by this module. */
#include <crypto/aead.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>

#include "vpt1_gateway.h"
#include "vpt1_generation.h"
#include "vpt1_runtime.h"
#include "vpt1_ip.h"
#include "vpt1_wire.h"

#define VPT1_AEAD "rfc7539(chacha20,poly1305)"

/* dev is borrowed under RTNL. Generic per-net storage owns this context. */
struct vpt1_net {
	struct net_device *dev;
	u64 incarnation;
	struct vpt1_gw __rcu *gw; /* AY2 gateway; published and cleared under RTNL. */
};

struct vpt1_dev {
	atomic_long_t drops;
	atomic_long_t tx_packets;
	atomic_long_t tx_bytes;
	atomic_long_t rx_packets;
	atomic_long_t rx_bytes;
	bool changing_mtu; /* RTNL serialized; only a configuration commit sets it. */
};

static unsigned int vpt1_net_id;

struct vpt1_gw *vpt1_net_gw_get(struct net *net)
{
	struct vpt1_net *vn = net_generic(net, vpt1_net_id);
	struct vpt1_gw *gw;

	rcu_read_lock();
	gw = rcu_dereference(vn->gw);
	if (gw && !vpt1_gw_get(gw))
		gw = NULL;
	rcu_read_unlock();
	return gw;
}

struct vpt1_gw *vpt1_net_gw_ensure(struct net *net, u32 ifindex)
{
	struct vpt1_net *vn = net_generic(net, vpt1_net_id);
	struct vpt1_gw *gw;

	ASSERT_RTNL();
	/* A claim needs the namespace's vpn0, and IFINDEX must name exactly it. */
	if (!vn->dev || vn->dev->ifindex != ifindex)
		return ERR_PTR(-ENODEV);
	gw = rtnl_dereference(vn->gw);
	if (!gw) {
		gw = vpt1_gw_create(ifindex, ktime_get_boottime_ns());
		if (!gw)
			return ERR_PTR(-ENOMEM);
		vpt1_gw_set_net(gw, net);
		rcu_assign_pointer(vn->gw, gw);
	}
	if (!vpt1_gw_get(gw))
		return ERR_PTR(-ENODEV);
	return gw;
}

void vpt1_net_gw_unpublish(struct net *net, struct vpt1_gw *gw)
{
	struct vpt1_net *vn = net_generic(net, vpt1_net_id);

	ASSERT_RTNL();
	if (rtnl_dereference(vn->gw) == gw)
		RCU_INIT_POINTER(vn->gw, NULL);
}

/* RTNL held: unpublish, then close, drain and unpublish the gateway state. */
static void vpt1_net_gw_teardown(struct vpt1_net *vn)
{
	struct vpt1_gw *gw = rtnl_dereference(vn->gw);

	if (!gw)
		return;
	RCU_INIT_POINTER(vn->gw, NULL);
	vpt1_gw_destroy(gw, 0);
}
static atomic64_t vpt1_incarnation = ATOMIC64_INIT(0);

/*
 * Consumes skb on every path. With no gateway, no bound socket or no eligible peer the
 * packet is dropped (fail closed); otherwise it is sealed and sent by the data path.
 */
static netdev_tx_t vpt1_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct vpt1_dev *priv = netdev_priv(dev);
	struct vpt1_net *vn = net_generic(dev_net(dev), vpt1_net_id);
	unsigned int len = skb->len;
	struct vpt1_gw *gw;
	bool sent = false;

	rcu_read_lock();
	gw = rcu_dereference(vn->gw);
	if (gw)
		sent = vpt1_dp_xmit(gw, dev, skb);
	else
		dev_kfree_skb_any(skb);
	rcu_read_unlock();
	if (sent) {
		atomic_long_inc(&priv->tx_packets);
		atomic_long_add(len, &priv->tx_bytes);
	} else {
		atomic_long_inc(&priv->drops);
	}
	return NETDEV_TX_OK;
}

/* Records received and delivered to this device; the controller reads these to re-key on usage. */
void vpt1_dev_rx_add(struct net_device *dev, unsigned int bytes)
{
	struct vpt1_dev *priv = netdev_priv(dev);

	atomic_long_inc(&priv->rx_packets);
	atomic_long_add(bytes, &priv->rx_bytes);
}

static void vpt1_get_stats64(struct net_device *dev,
			     struct rtnl_link_stats64 *stats)
{
	struct vpt1_dev *priv = netdev_priv(dev);

	stats->tx_dropped = atomic_long_read(&priv->drops);
	stats->tx_packets = atomic_long_read(&priv->tx_packets);
	stats->tx_bytes = atomic_long_read(&priv->tx_bytes);
	stats->rx_packets = atomic_long_read(&priv->rx_packets);
	stats->rx_bytes = atomic_long_read(&priv->rx_bytes);
}

static int vpt1_change_mtu(struct net_device *dev, int mtu)
{
	struct vpt1_dev *priv = netdev_priv(dev);

	/* Reject manual overrides of the committed pair-cap minimum. The kernel
	 * may call us again to roll back a rejected NETDEV_CHANGEMTU notifier.
	 */
	if (!priv->changing_mtu)
		return -EPERM;
	WRITE_ONCE(dev->mtu, mtu);
	return 0;
}

int vpt1_dev_set_mtu(struct vpt1_gw *gw, u16 mtu)
{
	struct vpt1_net *vn = net_generic(vpt1_gw_net(gw), vpt1_net_id);
	struct vpt1_dev *priv;
	int err;

	ASSERT_RTNL();
	if (!vn->dev || vn->dev->ifindex != vpt1_gw_ifindex(gw))
		return -ENODEV;
	if (mtu < VPT1_INNER_CAP_MIN || mtu > VPT1_INNER_CAP_MAX)
		return -EINVAL;
	priv = netdev_priv(vn->dev);
	priv->changing_mtu = true;
	err = dev_set_mtu(vn->dev, mtu);
	priv->changing_mtu = false;
	return err;
}

static const struct net_device_ops vpt1_netdev_ops = {
	.ndo_start_xmit = vpt1_xmit,
	.ndo_get_stats64 = vpt1_get_stats64,
	.ndo_change_mtu = vpt1_change_mtu,
};

static void vpt1_setup(struct net_device *dev)
{
	struct vpt1_dev *priv = netdev_priv(dev);

	atomic_long_set(&priv->drops, 0);
	dev->netdev_ops = &vpt1_netdev_ops;
	dev->needs_free_netdev = true;
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP;
	dev->mtu = 1428;
	dev->min_mtu = 68;
	dev->max_mtu = 1428;
	dev->tx_queue_len = 0;
	dev->features = 0;
	dev->netns_immutable = true;
	dev->hw_features = 0;
	dev->vlan_features = 0;
	netif_carrier_off(dev);
}

/* RTNL serializes publication, explicit deletion and per-net cleanup. */
static int vpt1_newlink(struct net_device *dev,
			struct rtnl_newlink_params *params,
			struct netlink_ext_ack *extack)
{
	struct vpt1_net *vn = net_generic(dev_net(dev), vpt1_net_id);
	int err;

	ASSERT_RTNL();
	if (vn->dev) {
		NL_SET_ERR_MSG(extack, "one AY1 device per namespace");
		return -EEXIST;
	}
	err = register_netdevice(dev);
	if (err)
		return err;
	vn->dev = dev;
	return 0;
}

static void vpt1_dellink(struct net_device *dev, struct list_head *head)
{
	struct vpt1_net *vn = net_generic(dev_net(dev), vpt1_net_id);

	ASSERT_RTNL();
	/* Admission closes with the device, before it is unregistered. */
	vpt1_net_gw_teardown(vn);
	vn->dev = NULL;
	unregister_netdevice_queue(dev, head);
}

static struct rtnl_link_ops vpt1_link_ops = {
	.kind = "vpt1_ay1",
	.priv_size = sizeof(struct vpt1_dev),
	.setup = vpt1_setup,
	.newlink = vpt1_newlink,
	.dellink = vpt1_dellink,
};

/* No allocation, sleeping lock or authenticated state. Runs before shared defrag.
 * Noninitial fragments cannot identify a UDP port: all UDP fragments addressed
 * to the exact configured project underlay IP/interface are refused. Other local
 * addresses/interfaces and unfragmented non-project ports retain host policy.
 */
static unsigned int vpt1_outer_ingress(void *priv, struct sk_buff *skb,
				       const struct nf_hook_state *state)
{
	struct vpt1_net *vn = net_generic(state->net, vpt1_net_id);
	unsigned int verdict = NF_ACCEPT;
	u8 buffer[68]; /* Maximum IPv4 header plus UDP header. */
	const u8 *header;
	struct vpt1_gw *gw;
	unsigned int length, copied;
	__be32 destination;
	int offset = skb_network_offset(skb);

	if (!state->in || offset < 0 || offset > skb->len || skb->len - offset < 20)
		return NF_ACCEPT;
	length = skb->len - offset;
	copied = min_t(unsigned int, length, sizeof(buffer));
	header = skb_header_pointer(skb, offset, copied, buffer);
	if (!header || header[9] != IPPROTO_UDP)
		return NF_ACCEPT;
	memcpy(&destination, header + 16, sizeof(destination));
	rcu_read_lock();
	gw = rcu_dereference(vn->gw);
	if (gw && vpt1_gw_underlay_local(gw, state->in->ifindex, destination) &&
	    vpt1_ip_outer_guard(header, copied, length) < 0) {
		vpt1_gw_stat_inc(gw, VPT1_E_RX_MALFORMED);
		verdict = NF_DROP;
	}
	rcu_read_unlock();
	return verdict;
}

static const struct nf_hook_ops vpt1_outer_hook = {
	.hook = vpt1_outer_ingress,
	.pf = NFPROTO_IPV4,
	.hooknum = NF_INET_PRE_ROUTING,
	.priority = NF_IP_PRI_RAW_BEFORE_DEFRAG,
};

static int __net_init vpt1_net_init(struct net *net)
{
	struct vpt1_net *vn = net_generic(net, vpt1_net_id);
	int err;

	err = nf_register_net_hook(net, &vpt1_outer_hook);
	if (err)
		return err;
	vn->incarnation = atomic64_inc_return(&vpt1_incarnation);
	pr_info("vpt1_ay1: net_init ns=%u incarnation=%llu\n",
		net->ns.inum, vn->incarnation);
	return 0;
}

static void __net_exit vpt1_net_exit(struct net *net)
{
	struct vpt1_net *vn = net_generic(net, vpt1_net_id);

	rtnl_lock();
	vpt1_net_gw_teardown(vn);
	if (vn->dev) {
		struct net_device *dev = vn->dev;

		vn->dev = NULL;
		unregister_netdevice(dev);
	}
	rtnl_unlock();
	nf_unregister_net_hook(net, &vpt1_outer_hook);
	pr_info("vpt1_ay1: net_exit ns=%u incarnation=%llu\n",
		net->ns.inum, vn->incarnation);
}

static struct pernet_operations vpt1_pernet_ops = {
	.init = vpt1_net_init,
	.exit = vpt1_net_exit,
	.id = &vpt1_net_id,
	.size = sizeof(struct vpt1_net),
};

/* Process context; temporary transform is released on success and failure. */
static int vpt1_check_crypto(void)
{
	struct crypto_aead *tfm;
	int err;

	tfm = crypto_alloc_aead(VPT1_AEAD, 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	err = crypto_aead_setauthsize(tfm, 16);
	if (!err && crypto_aead_ivsize(tfm) != 12)
		err = -EINVAL;
	if (!err)
		pr_info("vpt1_ay1: aead=%s driver=%s iv=12 tag=16\n",
			VPT1_AEAD, crypto_aead_driver_name(tfm));
	crypto_free_aead(tfm);
	return err;
}

static int __init vpt1_init(void)
{
	int err;

	err = vpt1_check_crypto();
	if (err) {
		pr_err("vpt1_ay1: required AEAD unavailable/incompatible: %d\n",
		       err);
		return err;
	}
	err = vpt1_generation_init();
	if (err)
		return err;
	err = vpt1_gw_init();
	if (err)
		goto out_gen;
	vpt1_gw_set_ops(&vpt1_dp_ops);
	err = register_pernet_subsys(&vpt1_pernet_ops);
	if (err)
		goto out_gw;
	err = rtnl_link_register(&vpt1_link_ops);
	if (err)
		goto out_pernet;
	err = vpt1_netlink_init();
	if (err)
		goto out_link;
	pr_info("vpt1_ay1: ready; forwarding disabled\n");
	return 0;
out_link:
	rtnl_link_unregister(&vpt1_link_ops);
out_pernet:
	unregister_pernet_subsys(&vpt1_pernet_ops);
out_gw:
	vpt1_gw_exit();
out_gen:
	vpt1_generation_exit();
	return err;
}

static void __exit vpt1_exit(void)
{
	/* Contract section 7: close every gateway, then stop control requests. */
	vpt1_gw_close_all();
	vpt1_netlink_exit();
	rtnl_link_unregister(&vpt1_link_ops);
	unregister_pernet_subsys(&vpt1_pernet_ops);
	vpt1_gw_exit(); /* Includes the rcu_barrier() calls after every free is queued. */
	vpt1_gw_set_ops(NULL);
	vpt1_generation_drain();
	vpt1_generation_exit();
	pr_info("vpt1_ay1: unloaded gw=%ld peer=%ld cfg=%ld gen=%ld\n",
		vpt1_gw_live(), vpt1_peer_live(), vpt1_cfg_live(),
		vpt1_generation_live());
}

module_init(vpt1_init);
module_exit(vpt1_exit);
#ifdef VPT1_DEBUG
/* Debug builds only: stall the lease-expiry and owner-loss cleanup work. */
module_param_named(debug_stall_expiry, vpt1_dbg_stall_expiry.counter, int, 0644);
MODULE_PARM_DESC(debug_stall_expiry, "stall lease cleanup work (debug build only)");
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 AY1 per-net/device feasibility foundation");
MODULE_AUTHOR("VPNTunnel team");
