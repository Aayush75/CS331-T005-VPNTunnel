// SPDX-License-Identifier: GPL-2.0-only
/* Test-only API composition probe. Load only in a disposable guest. */
#include <linux/module.h>
#include <linux/netfilter_ipv4.h>
#include <net/genetlink.h>
#include <net/ip.h>
#include <net/netns/generic.h>
#include <net/udp_tunnel.h>

struct vpt1_probe_net {
	struct socket *sock;
};

static unsigned int probe_net_id;

/* UDP encapsulation callback consumes every skb. Never forwards a packet. */
static int probe_recv(struct sock *sk, struct sk_buff *skb)
{
	kfree_skb(skb);
	return 0;
}

static unsigned int probe_hook(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	return NF_ACCEPT;
}

static const struct nf_hook_ops probe_hooks[] = {
	{ .hook = probe_hook, .pf = NFPROTO_IPV4,
	  .hooknum = NF_INET_PRE_ROUTING, .priority = -450 },
	{ .hook = probe_hook, .pf = NFPROTO_IPV4,
	  .hooknum = NF_INET_PRE_ROUTING, .priority = -300 },
};

static int __net_init probe_net_init(struct net *net)
{
	struct udp_tunnel_sock_cfg tunnel_cfg = { .encap_type = 1,
		.encap_rcv = probe_recv };
	struct sockaddr_in addr = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = htons(51888) };
	struct vpt1_probe_net *pn = net_generic(net, probe_net_id);
	int err;

	err = sock_create_kern(net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &pn->sock);
	if (err)
		return err;
	/* Per-net init runs before userspace can configure loopback. */
	ip_sock_set_freebind(pn->sock->sk);
	err = kernel_bind(pn->sock, (struct sockaddr *)&addr, sizeof(addr));
	if (err) {
		sock_release(pn->sock);
		pn->sock = NULL;
		return err;
	}
	/* Match udp_sock_create4() with its default checksum setting. */
	pn->sock->sk->sk_no_check_tx = 1;
	setup_udp_tunnel_sock(net, pn->sock, &tunnel_cfg);
	err = nf_register_net_hooks(net, probe_hooks, ARRAY_SIZE(probe_hooks));
	if (err) {
		udp_tunnel_sock_release(pn->sock);
		pn->sock = NULL;
	}
	return err;
}

static void __net_exit probe_net_exit(struct net *net)
{
	struct vpt1_probe_net *pn = net_generic(net, probe_net_id);

	nf_unregister_net_hooks(net, probe_hooks, ARRAY_SIZE(probe_hooks));
	udp_tunnel_sock_release(pn->sock);
	pn->sock = NULL;
}

static struct pernet_operations probe_pernet_ops = {
	.init = probe_net_init,
	.exit = probe_net_exit,
	.id = &probe_net_id,
	.size = sizeof(struct vpt1_probe_net),
};

static struct genl_family probe_family = {
	.name = "VPT1_AY1_PROBE",
	.version = 1,
	.netnsok = true,
	.module = THIS_MODULE,
};

static int __init probe_init(void)
{
	int err;

	err = genl_register_family(&probe_family);
	if (err)
		return err;
	err = register_pernet_subsys(&probe_pernet_ops);
	if (err)
		genl_unregister_family(&probe_family);
	return err;
}

static void __exit probe_exit(void)
{
	unregister_pernet_subsys(&probe_pernet_ops);
	genl_unregister_family(&probe_family);
	rcu_barrier();
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AY1 test-only UDP/Netfilter/Generic Netlink API probe");
