/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_RUNTIME_H
#define VPT1_RUNTIME_H

#include <linux/types.h>

#include "vpt1_gateway.h"

struct net;
struct vpt1_gw;

/*
 * Per-net gateway access, implemented by the foundation module's pernet storage.
 *
 * get: RCU lookup plus a handler reference (see vpt1_gw_get()); NULL when the
 * namespace has no gateway or it is stopping. Any process context, no RTNL.
 *
 * ensure: RTNL held. The namespace must have its vpn0 and @ifindex must name it.
 * Returns a referenced gateway, creating it on first use, or ERR_PTR.
 *
 * unpublish: RTNL held. Makes the gateway unreachable to new lookups; the caller
 * then runs vpt1_gw_destroy().
 */
struct vpt1_gw *vpt1_net_gw_get(struct net *net);
struct vpt1_gw *vpt1_net_gw_ensure(struct net *net, u32 ifindex);
void vpt1_net_gw_unpublish(struct net *net, struct vpt1_gw *gw);

struct net_device;
struct sk_buff;
/* The thin data path (datapath.c): gateway hooks and the vpn0 transmit entry point. */
extern const struct vpt1_gw_ops vpt1_dp_ops;
bool vpt1_dp_xmit(struct vpt1_gw *gw, struct net_device *dev, struct sk_buff *skb);
void vpt1_dev_rx_add(struct net_device *dev, unsigned int bytes);
/* COMMIT only: process context, RTNL before cfg_lock; failure rolls back MTU. */
int vpt1_dev_set_mtu(struct vpt1_gw *gw, u16 mtu);
/* Process context: send one INIT/RESPONSE from the gateway's bound socket to a peer. */
int vpt1_dp_send_exchange(struct vpt1_gw *gw, unsigned int slot, const void *buf, size_t len);

int vpt1_netlink_init(void);
void vpt1_netlink_exit(void);

/* Unicast EXCHANGE_RX (command 128) to the owner. For the transport code (AY5). */
int vpt1_netlink_exchange_rx(struct net *net, struct vpt1_gw *gw, u8 slot,
			     const struct vpt1_incarnation *inc, const void *bytes,
			     size_t len);
/* REKEY_GRANTED / REKEY_NEEDED (129, 130) to the owner. */
void vpt1_netlink_rekey_event(struct net *net, struct vpt1_gw *gw, u8 slot,
			      const struct vpt1_incarnation *inc, enum vpt1_ctl_event event,
			      const u8 *attempt);

#endif /* VPT1_RUNTIME_H */
