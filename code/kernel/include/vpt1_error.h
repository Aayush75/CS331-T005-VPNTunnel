/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_ERROR_H
#define VPT1_ERROR_H

#include <linux/types.h>
#include <linux/time64.h>

#define VPT1_ERROR_RETENTION_NS (10ULL * NSEC_PER_SEC)
#define VPT1_ERROR_PREFIX 64U
#define VPT1_ERROR_CONTEXTS 256U

enum vpt1_recent_kind {
	VPT1_RECENT_INNER = 1,
	VPT1_RECENT_OUTER = 2,
};

/* Value only, fitting a 128-byte allocator bucket on supported targets. The
 * canonical adapter owns the counted peer/incarnation reference, a maximum of
 * 256 entries of each kind per peer, locking, charging and invalidation.
 * No pointers, allocation, timer, permission or second peer object lives here.
 */
struct vpt1_recent_packet {
	u64 created_ns;
	u32 rule_id;
	u16 prefix_len;
	u8 kind;
	u8 reserved;
	u8 prefix[VPT1_ERROR_PREFIX];
};

/* Remember complete validated bytes, after the caller checks source ownership,
 * current rule/initiation eligibility and holds the canonical context. Inner
 * requires its saved nonzero rule ID; outer must be checksummed IPv4/UDP.
 * Output is unchanged on error. No witness or output permission is created.
 */
int vpt1_recent_save(struct vpt1_recent_packet *out, u8 kind, u32 rule_id,
		     const u8 *packet, size_t length, u64 now_ns);

/* Borrow the complete IPv4 ICMP error and a held recent context. Returns zero
 * for an exact inner association, one for a merely plausible OUTER path event,
 * or negative errno. An outer result never authorizes an inner quotation or
 * authenticates the router/peer, even if the quotation contains a counter.
 * Caller must reject ambiguous table matches, reload CURRENT saved-rule policy
 * and perform rate-limited final admission. TTL/checksum and ECN can change in
 * transit; other saved bytes must match. The 10-second deadline never refreshes.
 */
int vpt1_recent_match(const struct vpt1_recent_packet *recent, const u8 *error,
		      size_t length, u64 now_ns);

#endif /* VPT1_ERROR_H */
