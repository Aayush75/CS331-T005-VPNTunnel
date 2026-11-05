/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_SNAPSHOT_H
#define VPT1_SNAPSHOT_H

#include <linux/types.h>

#include "vpt1_gateway.h"

/*
 * PROPOSED nested layout for VPT1_ATTR_SNAPSHOT (Netlink ABI v1 does not fix the
 * member IDs yet; Raj and Aayush settle them with the per-command table, see
 * docs/AY2_RUNTIME.md). Members are plain (not NLA_F_NESTED) fixed-size byte
 * records, all integers big-endian, decoded field by field and never copied as a
 * native struct. Local record, once; peer records at most 4; rule records at most
 * 128. Prefix = address[4] length[1].
 *
 *   LOCAL (58): lan_ifindex[4] underlay_ifindex[4] lan prefix[5]
 *               underlay prefix[5] lan_address[4] underlay_address[4]
 *               local_key[32]
 *   PEER  (63): slot[1] role[1] pmtu[2] endpoint[4] port[2] key[32]
 *               prefix_count[1] prefixes[4 x 5]
 *   RULE  (21): id[4] slot[1] direction[1] protocol[1] port_first[2]
 *               port_last[2] source prefix[5] destination prefix[5]
 */
#define VPT1_SNAP_LOCAL 1
#define VPT1_SNAP_PEER 2
#define VPT1_SNAP_RULE 3

#define VPT1_SNAP_LOCAL_LEN 58
#define VPT1_SNAP_PEER_LEN 63
#define VPT1_SNAP_RULE_LEN 21

/* Reply members (also PROPOSED): STATUS and COUNTERS nested contents. */
#define VPT1_STATUS_OWNED 1 /* u8 */
#define VPT1_STATUS_STOPPING 2 /* u8 */
#define VPT1_STATUS_OWNER_EPOCH 3 /* u64 */
#define VPT1_STATUS_LEASE_MS 4 /* u64 */
#define VPT1_STATUS_REVISION 5 /* u64 */
#define VPT1_STATUS_STAGED_REVISION 6 /* u64, only when staged */
#define VPT1_STATUS_PEER 7 /* nested, one per configured peer */
#define VPT1_STATUS_PEER_SLOT 1 /* u16 */
#define VPT1_STATUS_PEER_STATE 2 /* u8, enum vpt1_peer_state */
#define VPT1_STATUS_PEER_HAS_GEN 3 /* u8 */
#define VPT1_STATUS_PEER_PMTU 4 /* u16 */
#define VPT1_STATUS_PEER_ENDPOINT 5 /* 4 bytes, network order */
#define VPT1_STATUS_PEER_PORT 6 /* u16 */
#define VPT1_STATUS_PEER_INCARNATION 7 /* 16 bytes */
/* COUNTERS members: type = counter index + 1 (enum vpt1_reason, then events), u64. */

/* data/len is the payload of the SNAPSHOT attribute. Fills out, never partially
 * trusted: on error the caller discards it.
 */
int vpt1_snapshot_decode(const void *data, int len, struct vpt1_snapshot *out);

#endif /* VPT1_SNAPSHOT_H */
