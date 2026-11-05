// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <net/checksum.h>
#include <net/ip.h>
#include "vpt1_gateway.h"
#include "vpt1_ip.h"
#include "vpt1_wire.h"

#define IP_LEN 20U
#define UDP_LEN 8U
#define TCP_LEN 20U
#define PROJECT_PORT 51888U

static_assert(IP_LEN + UDP_LEN + VPT1_RECORD_EXPANSION == VPT1_IP_EXPANSION);

u16 vpt1_ip_inner_cap(u16 pair_pmtu)
{
	return pair_pmtu >= 1280 && pair_pmtu <= 1500 ? pair_pmtu - VPT1_IP_EXPANSION : 0;
}

int vpt1_ip_lengths(u16 pair_pmtu, size_t inner_length, struct vpt1_ip_lengths *out)
{
	struct vpt1_ip_lengths result;
	u16 cap = vpt1_ip_inner_cap(pair_pmtu);

	if (!out || !cap)
		return -EINVAL;
	if (inner_length < IP_LEN || inner_length > cap)
		return -EMSGSIZE;
	result.udp_payload = inner_length + VPT1_RECORD_EXPANSION;
	result.udp_total = result.udp_payload + UDP_LEN;
	result.outer_total = result.udp_total + IP_LEN;
	*out = result;
	return 0;
}

static int header_valid(const u8 *packet, size_t length)
{
	if (!packet || length < IP_LEN || length > 65535)
		return -EMSGSIZE;
	if (packet[0] != 0x45 || get_unaligned_be16(packet + 2) != length ||
	    (get_unaligned_be16(packet + 6) & (IP_CE | IP_MF | IP_OFFSET)) || !packet[8])
		return -EPROTO;
	if (csum_fold(csum_partial(packet, IP_LEN, 0)))
		return -EBADMSG;
	return 0;
}

int vpt1_ip_validate(const u8 *packet, size_t length, struct vpt1_pkt_info *out)
{
	struct vpt1_pkt_info info = {};
	unsigned int transport_len, tcp_header;
	const u8 *transport;
	int err;

	if (!out)
		return -EINVAL;
	err = header_valid(packet, length);
	if (err)
		return err;
	memcpy(&info.src, packet + 12, sizeof(info.src));
	memcpy(&info.dst, packet + 16, sizeof(info.dst));
	info.proto = packet[9];
	transport = packet + IP_LEN;
	transport_len = length - IP_LEN;
	switch (info.proto) {
	case IPPROTO_TCP:
		if (transport_len < TCP_LEN)
			return -EMSGSIZE;
		tcp_header = (transport[12] >> 4) * 4U;
		if (tcp_header < TCP_LEN || tcp_header > transport_len)
			return -EPROTO;
		if (csum_tcpudp_magic(info.src, info.dst, transport_len, info.proto,
				      csum_partial(transport, transport_len, 0)))
			return -EBADMSG;
		info.tcp_flags = transport[13];
		break;
	case IPPROTO_UDP:
		if (transport_len < UDP_LEN)
			return -EMSGSIZE;
		if (get_unaligned_be16(transport + 4) != transport_len)
			return -EPROTO;
		if (get_unaligned_be16(transport + 6) &&
		    csum_tcpudp_magic(info.src, info.dst, transport_len, info.proto,
				      csum_partial(transport, transport_len, 0)))
			return -EBADMSG;
		break;
	case IPPROTO_ICMP:
		if (transport_len < 8)
			return -EMSGSIZE;
		if ((transport[0] != ICMP_ECHO && transport[0] != ICMP_ECHOREPLY) || transport[1])
			return -EPROTONOSUPPORT;
		if (csum_fold(csum_partial(transport, transport_len, 0)))
			return -EBADMSG;
		info.icmp_type = transport[0];
		info.icmp_code = transport[1];
		*out = info;
		return 0;
	default:
		return -EPROTONOSUPPORT;
	}
	memcpy(&info.sport, transport, sizeof(info.sport));
	memcpy(&info.dport, transport + 2, sizeof(info.dport));
	*out = info;
	return 0;
}

int vpt1_ip_skb_linear(struct sk_buff *skb)
{
	u8 header[IP_LEN];
	unsigned int checksum_offset;
	int err;

	if (!skb || skb->protocol != htons(ETH_P_IP) || skb_network_offset(skb))
		return -EINVAL;
	if (skb_is_gso(skb))
		return -EOPNOTSUPP;
	if (skb->len < IP_LEN || skb->len > 65535)
		return -EMSGSIZE;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		if (skb_copy_bits(skb, 0, header, sizeof(header)) || header[0] != 0x45 ||
		    skb_checksum_start_offset(skb) != IP_LEN)
			return -EPROTO;
		switch (header[9]) {
		case IPPROTO_TCP:
			checksum_offset = 16;
			break;
		case IPPROTO_UDP:
			checksum_offset = 6;
			break;
		case IPPROTO_ICMP:
			checksum_offset = 2;
			break;
		default:
			return -EPROTONOSUPPORT;
		}
		if (skb->csum_offset != checksum_offset || skb->len < IP_LEN + checksum_offset + 2)
			return -EPROTO;
	}
	/* skb_checksum_help requires its checksum field in the linear head. */
	if (skb_linearize(skb))
		return -ENOMEM;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		err = skb_checksum_help(skb);
		if (err)
			return err;
	}
	return 0;
}

int vpt1_ip_skb_prepare(struct sk_buff *skb, struct vpt1_pkt_info *out)
{
	int err;

	if (!out)
		return -EINVAL;
	err = vpt1_ip_skb_linear(skb);
	return err ? err : vpt1_ip_validate(skb->data, skb->len, out);
}

int vpt1_ip_ecn_merge(u8 inner_tos, u8 outer_tos, u8 *out)
{
	u8 inner = inner_tos & 3;
	u8 outer = outer_tos & 3;
	u8 merged = inner;

	if (!out)
		return -EINVAL;
	if (!inner) {
		if (outer == 3)
			return -EPROTO;
		*out = inner_tos;
		return outer ? 1 : 0;
	}
	if (outer == 3 || inner == 3)
		merged = 3;
	else if (outer == 1 || inner == 1)
		merged = 1;
	*out = (inner_tos & 0xfc) | merged;
	return 0;
}

/* The header is already validated: merge the outer mark and repair the checksum. */
static int ecn_apply_checked(u8 *packet, u8 outer_tos)
{
	__sum16 checksum;
	u8 tos;
	int err;

	err = vpt1_ip_ecn_merge(packet[1], outer_tos, &tos);
	if (err < 0)
		return err;
	if (tos != packet[1]) {
		packet[1] = tos;
		put_unaligned_be16(0, packet + 10);
		checksum = csum_fold(csum_partial(packet, IP_LEN, 0));
		memcpy(packet + 10, &checksum, sizeof(checksum));
	}
	return err;
}

int vpt1_ip_ecn_apply(u8 *packet, size_t length, u8 outer_tos)
{
	int err = header_valid(packet, length);

	return err ? err : ecn_apply_checked(packet, outer_tos);
}

int vpt1_ip_frag_ecn_apply(u8 *packet, size_t length, u8 outer_tos)
{
	int err = vpt1_ip_fragment_check(packet, length);

	return err ? err : ecn_apply_checked(packet, outer_tos);
}

/* Fragments and output fragmentation -------------------------------------------------- */

bool vpt1_ip_is_fragment(const u8 *packet, size_t length)
{
	return packet && length >= IP_LEN &&
	       (get_unaligned_be16(packet + 6) & (IP_MF | IP_OFFSET));
}

int vpt1_ip_fragment_check(const u8 *packet, size_t length)
{
	u16 flags;
	u32 payload, offset;

	if (!packet || length <= IP_LEN || length > 65535)
		return -EMSGSIZE;
	flags = get_unaligned_be16(packet + 6);
	if (packet[0] != 0x45 || get_unaligned_be16(packet + 2) != length || !packet[8] ||
	    !(flags & (IP_MF | IP_OFFSET)) || (flags & (IP_CE | IP_DF)))
		return -EPROTO;
	if (packet[9] != IPPROTO_TCP && packet[9] != IPPROTO_UDP && packet[9] != IPPROTO_ICMP)
		return -EPROTONOSUPPORT;
	if (csum_fold(csum_partial(packet, IP_LEN, 0)))
		return -EBADMSG;
	payload = length - IP_LEN;
	offset = (flags & IP_OFFSET) * 8U;
	if ((flags & IP_MF) && (payload % 8))
		return -EPROTO;
	if (offset + payload > 65535 - IP_LEN)
		return -EMSGSIZE;
	return 0;
}

static unsigned int fragment_payload(unsigned int mtu)
{
	return mtu > IP_LEN + 8 ? (mtu - IP_LEN) & ~7U : 0;
}

unsigned int vpt1_ip_fragment_count(size_t length, unsigned int mtu)
{
	unsigned int each = fragment_payload(mtu);

	if (length <= IP_LEN || length > 65535)
		return 0;
	if (length <= mtu)
		return 1;
	return each ? DIV_ROUND_UP(length - IP_LEN, each) : 0;
}

int vpt1_ip_fragment_build(const u8 *packet, size_t length, unsigned int mtu, unsigned int index,
			   u8 *out)
{
	unsigned int each = fragment_payload(mtu), count = vpt1_ip_fragment_count(length, mtu);
	unsigned int offset, size;
	__sum16 checksum;
	u16 flags;

	if (!packet || !out || !count || index >= count || packet[0] != 0x45 ||
	    get_unaligned_be16(packet + 2) != length)
		return -EINVAL;
	flags = get_unaligned_be16(packet + 6);
	if (flags & (IP_MF | IP_OFFSET | IP_CE))
		return -EPROTO; /* Only a complete packet is fragmented. */
	if (count == 1) {
		memcpy(out, packet, length);
		return length;
	}
	if (flags & IP_DF)
		return -EMSGSIZE; /* Never clear DF to hide a size failure. */
	offset = index * each;
	size = min_t(unsigned int, each, length - IP_LEN - offset);
	memcpy(out, packet, IP_LEN);
	memcpy(out + IP_LEN, packet + IP_LEN + offset, size);
	put_unaligned_be16(IP_LEN + size, out + 2);
	put_unaligned_be16((offset / 8) | (index + 1 < count ? IP_MF : 0), out + 6);
	put_unaligned_be16(0, out + 10);
	checksum = csum_fold(csum_partial(out, IP_LEN, 0));
	memcpy(out + 10, &checksum, sizeof(checksum));
	return IP_LEN + size;
}

/* Associated ICMP errors ---------------------------------------------------------------- */

bool vpt1_ip_is_icmp_error(const u8 *packet, size_t length)
{
	return packet && length >= IP_LEN + 8 && packet[9] == IPPROTO_ICMP &&
	       (packet[IP_LEN] == ICMP_DEST_UNREACH || packet[IP_LEN] == ICMP_TIME_EXCEEDED);
}

int vpt1_ip_error_validate(const u8 *packet, size_t length, struct vpt1_pkt_info *out)
{
	struct vpt1_pkt_info info = {};
	u8 type, code;

	if (!out)
		return -EINVAL;
	/* The quotation must hold a whole IPv4 header and eight transport bytes (RFC 792). */
	if (!packet || length < 56 || length > 576)
		return -EMSGSIZE;
	if (packet[0] != 0x45 || get_unaligned_be16(packet + 2) != length || !packet[8] ||
	    packet[9] != IPPROTO_ICMP ||
	    (get_unaligned_be16(packet + 6) & (IP_CE | IP_MF | IP_OFFSET)))
		return -EPROTO;
	if (csum_fold(csum_partial(packet, IP_LEN, 0)) ||
	    csum_fold(csum_partial(packet + IP_LEN, length - IP_LEN, 0)))
		return -EBADMSG;
	type = packet[IP_LEN];
	code = packet[IP_LEN + 1];
	if (!((type == ICMP_DEST_UNREACH && code <= NR_ICMP_UNREACH) ||
	      (type == ICMP_TIME_EXCEEDED && code <= ICMP_EXC_FRAGTIME)))
		return -EPROTONOSUPPORT;
	memcpy(&info.src, packet + 12, sizeof(info.src));
	memcpy(&info.dst, packet + 16, sizeof(info.dst));
	info.proto = IPPROTO_ICMP;
	info.icmp_type = type;
	info.icmp_code = code;
	*out = info;
	return 0;
}

int vpt1_ip_outer_guard(const u8 *header, size_t available, size_t packet_length)
{
	unsigned int ihl;
	u16 flags;

	if (!header || available < IP_LEN || packet_length < IP_LEN || (header[0] >> 4) != 4)
		return -EPROTO;
	if (header[9] != IPPROTO_UDP)
		return 0;
	flags = get_unaligned_be16(header + 6);
	if (flags & (IP_CE | IP_MF | IP_OFFSET))
		return -EPROTO;
	ihl = (header[0] & 15) * 4U;
	if (ihl < IP_LEN || ihl > available || available - ihl < UDP_LEN)
		return -EMSGSIZE;
	if (get_unaligned_be16(header + ihl + 2) != PROJECT_PORT)
		return 0;
	if (ihl != IP_LEN || get_unaligned_be16(header + 2) != packet_length ||
	    packet_length > 1500 || packet_length < IP_LEN + UDP_LEN)
		return -EPROTO;
	return 1;
}
