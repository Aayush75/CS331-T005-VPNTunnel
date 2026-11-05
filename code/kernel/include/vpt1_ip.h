/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_IP_H
#define VPT1_IP_H

#include <linux/types.h>

#define VPT1_IP_EXPANSION 72U

struct sk_buff;
struct vpt1_pkt_info;

struct vpt1_ip_lengths {
	u16 udp_payload;
	u16 udp_total;
	u16 outer_total;
};

/* Pure values, no allocation or permission. Output remains unchanged on error. */
u16 vpt1_ip_inner_cap(u16 pair_pmtu);
int vpt1_ip_lengths(u16 pair_pmtu, size_t inner_length, struct vpt1_ip_lengths *out);

/* Borrow complete, stable, possibly unaligned linear inner IPv4 bytes.
 * Validate the header AND supported transport checksum/extent before extracting
 * policy fields. IPv4 options/fragments and ICMP other than echo are not admitted
 * by this thin path. TCP state/witness and ICMP-error association are separate.
 * IPv4 UDP checksum zero is legal; a nonzero checksum must verify.
 */
int vpt1_ip_validate(const u8 *packet, size_t length, struct vpt1_pkt_info *out);

/* Non-sleeping, borrows skb (never consumes). May linearize and complete a valid
 * CHECKSUM_PARTIAL checksum, so reload pointers afterward. Does not trust
 * CHECKSUM_UNNECESSARY for these inner bytes. GSO requires bounded segmentation
 * BEFORE this per-packet API and returns EOPNOTSUPP here, never one huge record.
 */
int vpt1_ip_skb_prepare(struct sk_buff *skb, struct vpt1_pkt_info *out);
/* The part of vpt1_ip_skb_prepare() before validation: protocol, GSO, length, checksum
 * completion and linearization. The caller then classifies the bytes (fragment, ICMP error,
 * ordinary packet).
 */
int vpt1_ip_skb_linear(struct sk_buff *skb);

/* Fragments (HR3 live adapter). A fragment is an IPv4 packet with MF set or a nonzero offset.
 * is_fragment looks at the flags only. fragment_check validates one fragment on its own, before
 * it is queued for normalization: version/IHL 5, exact length, header checksum, nonzero TTL, DF and
 * the reserved flag clear (a DF fragment is not part of profile 1), TCP/UDP/ICMP only, a nonempty
 * payload, and a multiple of eight bytes unless it is the last. It never reads a transport header.
 * frag_ecn_apply is vpt1_ip_ecn_apply for a fragment. Each fragment carries its own outer ECN
 * mark, the cache merges them (docs/HR3.md).
 */
bool vpt1_ip_is_fragment(const u8 *packet, size_t length);
int vpt1_ip_fragment_check(const u8 *packet, size_t length);
int vpt1_ip_frag_ecn_apply(u8 *packet, size_t length, u8 outer_tos);

/* Output fragmentation of one COMPLETE, unfragmented IPv4 packet into fragments of at most @mtu
 * bytes (DF clear only; a DF packet larger than @mtu is -EMSGSIZE). fragment_count gives 1 when it
 * already fits. fragment_build writes fragment @index (header and payload, checksum repaired) into
 * @out, which must hold @mtu bytes, and returns its length. Pure functions, no allocation.
 */
unsigned int vpt1_ip_fragment_count(size_t length, unsigned int mtu);
int vpt1_ip_fragment_build(const u8 *packet, size_t length, unsigned int mtu, unsigned int index,
			   u8 *out);

/* Associated ICMP error (TECHNICAL_DESIGN 10.3): complete, checksummed IPv4 destination-unreachable
 * or time-exceeded error with no options or fragmentation. Fills source/destination and the ICMP
 * type/code; the quoted bytes are matched against a recent context by vpt1_recent_match().
 */
bool vpt1_ip_is_icmp_error(const u8 *packet, size_t length);
int vpt1_ip_error_validate(const u8 *packet, size_t length, struct vpt1_pkt_info *out);

/* RFC 6040 default egress: 0 normal, 1 diagnostic anomaly, negative = drop.
 * Only ECN bits merge; preserve inner DSCP. Not-ECT + outer CE drops unchanged.
 * apply borrows writable validated IPv4 bytes and repairs the IPv4 checksum.
 * It does not decrement TTL or validate/authenticate the outer carrier.
 */
int vpt1_ip_ecn_merge(u8 inner_tos, u8 outer_tos, u8 *out);
int vpt1_ip_ecn_apply(u8 *packet, size_t length, u8 outer_tos);

/* For UDP addressed to this gateway's configured underlay IP/interface only.
 * Reject fragments before shared defrag (port is unknowable in later pieces).
 * For unfragmented traffic: 0 unrelated port, 1 valid project header, negative
 * malformed/unsupported. UDP checksum verification remains the socket's job.
 */
int vpt1_ip_outer_guard(const u8 *header, size_t available, size_t packet_length);

#endif /* VPT1_IP_H */
