// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/build_bug.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <net/checksum.h>
#include <net/ip.h>
#include "vpt1_error.h"
#include "vpt1_gateway.h"
#include "vpt1_ip.h"

int vpt1_recent_save(struct vpt1_recent_packet *out, u8 kind, u32 rule_id,
		     const u8 *packet, size_t length, u64 now_ns)
{
	struct vpt1_recent_packet value = {};
	struct vpt1_pkt_info pi;
	int err;

	static_assert(sizeof(value) <= 128);
	if (!out || (kind != VPT1_RECENT_INNER && kind != VPT1_RECENT_OUTER) ||
	    (kind == VPT1_RECENT_INNER && !rule_id))
		return -EINVAL;
	err = vpt1_ip_validate(packet, length, &pi);
	if (err)
		return err;
	if (kind == VPT1_RECENT_OUTER &&
	    (pi.proto != IPPROTO_UDP || length > 1500 || !get_unaligned_be16(packet + 26)))
		return -EPROTO;
	value.created_ns = now_ns;
	value.rule_id = rule_id;
	value.kind = kind;
	value.prefix_len = min_t(size_t, length, sizeof(value.prefix));
	memcpy(value.prefix, packet, value.prefix_len);
	*out = value;
	return 0;
}

int vpt1_recent_match(const struct vpt1_recent_packet *recent, const u8 *error,
		      size_t length, u64 now_ns)
{
	const u8 *quote;
	size_t compared, i;
	u8 type, code;

	if (!recent || !error || recent->reserved || recent->prefix_len < 28 ||
	    recent->prefix_len > sizeof(recent->prefix) ||
	    (recent->kind == VPT1_RECENT_INNER && !recent->rule_id) ||
	    (recent->kind != VPT1_RECENT_INNER && recent->kind != VPT1_RECENT_OUTER))
		return -EINVAL;
	if (now_ns < recent->created_ns || now_ns - recent->created_ns >= VPT1_ERROR_RETENTION_NS)
		return -ESTALE;
	/* Full error, unlike its deliberately truncated quoted packet. */
	if (length < 56 || length > 65535 || error[0] != 0x45 ||
	    get_unaligned_be16(error + 2) != length || !error[8] ||
	    error[9] != IPPROTO_ICMP ||
	    (get_unaligned_be16(error + 6) & (IP_CE | IP_MF | IP_OFFSET)))
		return -EPROTO;
	if (csum_fold(csum_partial(error, 20, 0)) ||
	    csum_fold(csum_partial(error + 20, length - 20, 0)))
		return -EBADMSG;
	type = error[20];
	code = error[21];
	if (!((type == ICMP_DEST_UNREACH && code <= 15) ||
	      (type == ICMP_TIME_EXCEEDED && code <= 1)))
		return -EPROTONOSUPPORT;
	if (memcmp(error + 16, recent->prefix + 12, 4))
		return -ENOENT; /* The error returns to this recorded packet's source. */
	quote = error + 28;
	compared = min_t(size_t, length - 28, recent->prefix_len);
	for (i = 0; i < compared; i++) {
		if (i == 8 || i == 10 || i == 11)
			continue; /* Forwarded TTL and IPv4 header checksum. */
		if (i == 1) {
			if ((quote[i] & 0xfc) != (recent->prefix[i] & 0xfc))
				return -ENOENT;
		} else if (quote[i] != recent->prefix[i]) {
			return -ENOENT;
		}
	}
	return recent->kind == VPT1_RECENT_OUTER ? 1 : 0;
}
