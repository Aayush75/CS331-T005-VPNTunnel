// SPDX-License-Identifier: GPL-2.0-only
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <net/checksum.h>
#include <net/ip.h>
#include "vpt1_gateway.h"
#include "vpt1_ip.h"
#include "vpt1_error.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define ip_test kunit
#define EXPECT(t, c) KUNIT_EXPECT_TRUE_MSG(t, c, "%s:%d", __func__, __LINE__)
#else
struct ip_test { unsigned int assertions, failures; };
static void expect(struct ip_test *t, bool c, unsigned int line)
{
	t->assertions++;
	if (!c) {
		t->failures++;
		pr_err("vpt1_ip_test: line %u failed\n", line);
	}
}

#define EXPECT(t, c) expect(t, c, __LINE__)
#endif

/* Independent bytewise Internet checksum; deliberately not the kernel helpers. */
static u16 checksum(const u8 *p, unsigned int len, u32 seed)
{
	while (len > 1) {
		seed += ((u16)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		seed += (u16)p[0] << 8;
	while (seed >> 16)
		seed = (seed & 65535) + (seed >> 16);
	return ~seed;
}

static void repair_ip(u8 *p)
{
	p[10] = 0;
	p[11] = 0;
	put_unaligned_be16(checksum(p, 20, 0), p + 10);
}

static u32 pseudo(const u8 *p, unsigned int len)
{
	return 0x0a00 + 1 + 0x0a00 + 2 + p[9] + len;
}

static unsigned int packet(u8 *p, u8 proto, unsigned int payload)
{
	unsigned int l4 = (proto == IPPROTO_TCP ? 20 : 8) + payload;
	unsigned int len = 20 + l4, i, off;

	memset(p, 0, len);
	p[0] = 0x45;
	p[1] = 0xa2;
	put_unaligned_be16(len, p + 2);
	p[6] = 0x40;
	p[8] = 63;
	p[9] = proto;
	p[12] = 10;
	p[16] = 10;
	p[15] = 1;
	p[19] = 2;
	for (i = len - payload; i < len; i++)
		p[i] = i;
	if (proto == IPPROTO_ICMP) {
		p[20] = ICMP_ECHO;
		off = 22;
	} else {
		put_unaligned_be16(12345, p + 20);
		put_unaligned_be16(9090, p + 22);
		if (proto == IPPROTO_TCP) {
			p[32] = 0x50;
			p[33] = 2;
			off = 36;
		} else {
			put_unaligned_be16(l4, p + 24);
			off = 26;
		}
	}
	put_unaligned_be16(checksum(p + 20, l4,
				    proto == IPPROTO_ICMP ? 0 : pseudo(p, l4)), p + off);
	repair_ip(p);
	return len;
}

static void lengths_and_caps(struct ip_test *t)
{
	struct vpt1_ip_lengths out, saved = { 7, 8, 9 };
	unsigned int p, n;

	for (p = 1280; p <= 1500; p++) {
		EXPECT(t, vpt1_ip_inner_cap(p) == p - 72);
		for (n = p - 73; n <= p - 71; n++) {
			out = saved;
			if (n > p - 72) {
				EXPECT(t, vpt1_ip_lengths(p, n, &out) == -EMSGSIZE);
				EXPECT(t, !memcmp(&out, &saved, sizeof(out)));
			} else {
				EXPECT(t, !vpt1_ip_lengths(p, n, &out));
				EXPECT(t, out.udp_payload == n + 44);
				EXPECT(t, out.udp_total == n + 52 && out.outer_total == n + 72);
			}
		}
	}
	EXPECT(t, !vpt1_ip_inner_cap(1279) && !vpt1_ip_inner_cap(1501));
	out = saved;
	EXPECT(t, vpt1_ip_lengths(0, 20, &out) == -EINVAL);
	EXPECT(t, !memcmp(&out, &saved, sizeof(out)));
	EXPECT(t, vpt1_ip_lengths(1500, SIZE_MAX, &out) == -EMSGSIZE);
	EXPECT(t, vpt1_ip_lengths(1500, 19, &out) == -EMSGSIZE);
	EXPECT(t, vpt1_ip_lengths(1500, 20, NULL) == -EINVAL);
}

static void transport_checksums(struct ip_test *t)
{
	u8 storage[257], *p = storage + 1;
	struct vpt1_pkt_info out, saved;
	unsigned int proto, payload, len;
	const u8 protocols[] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };

	memset(&saved, 0x5a, sizeof(saved));
	for (proto = 0; proto < ARRAY_SIZE(protocols); proto++) {
		for (payload = 1; payload <= 127; payload++) {
			len = packet(p, protocols[proto], payload);
			EXPECT(t, !vpt1_ip_validate(p, len, &out));
			EXPECT(t, out.proto == protocols[proto]);
			p[len - 1] ^= 1;
			out = saved;
			EXPECT(t, vpt1_ip_validate(p, len, &out) == -EBADMSG);
			EXPECT(t, !memcmp(&out, &saved, sizeof(out)));
		}
	}
	len = packet(p, IPPROTO_UDP, 7);
	p[26] = 0;
	p[27] = 0;
	EXPECT(t, !vpt1_ip_validate(p, len, &out)); /* IPv4 UDP zero is legal. */
	p[25]--;
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTO);
	len = packet(p, IPPROTO_TCP, 7);
	p[32] = 0x40;
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTO);
	p[32] = 0xf0;
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTO);
	len = packet(p, IPPROTO_TCP, 7);
	p[32] = 0x60; /* A valid four-byte TCP option area is admitted. */
	memset(p + 40, 1, 4);
	p[36] = 0;
	p[37] = 0;
	put_unaligned_be16(checksum(p + 20, len - 20, pseudo(p, len - 20)), p + 36);
	/* The SYN flag byte is reported. */
	EXPECT(t, !vpt1_ip_validate(p, len, &out) && out.tcp_flags == 2);
	p[15] ^= 1;
	repair_ip(p);
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EBADMSG);
	len = packet(p, IPPROTO_ICMP, 7);
	p[20] = ICMP_DEST_UNREACH;
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTONOSUPPORT);
	EXPECT(t, vpt1_ip_validate(NULL, len, &out) == -EMSGSIZE);
	EXPECT(t, vpt1_ip_validate(p, len, NULL) == -EINVAL);
}

static void ipv4_boundaries(struct ip_test *t)
{
	u8 p[128];
	struct vpt1_pkt_info out;
	unsigned int len, i;
	const u16 flags[] = { 0x8000, 0x2000, 1, 0x3fff };

	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		len = packet(p, IPPROTO_UDP, 8);
		put_unaligned_be16(flags[i], p + 6);
		repair_ip(p);
		EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTO);
	}
	len = packet(p, IPPROTO_UDP, 8);
	p[0] = 0x46;
	repair_ip(p);
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTO);
	len = packet(p, IPPROTO_UDP, 8);
	p[10] ^= 1;
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EBADMSG);
	len = packet(p, IPPROTO_UDP, 8);
	p[8] = 0;
	repair_ip(p);
	EXPECT(t, vpt1_ip_validate(p, len, &out) == -EPROTO);
	EXPECT(t, vpt1_ip_validate(p, len - 1, &out) == -EPROTO);
	EXPECT(t, vpt1_ip_validate(p, 19, &out) == -EMSGSIZE);
	EXPECT(t, vpt1_ip_validate(p, 65536, &out) == -EMSGSIZE);
}

static void ecn_table(struct ip_test *t)
{
	/* Rows inner NE/ECT1/ECT0/CE; columns outer NE/ECT1/ECT0/CE. */
	static const s8 table[4][4] = { { 0, 0, 0, -1 }, { 1, 1, 1, 3 },
					     { 2, 1, 2, 3 }, { 3, 3, 3, 3 } };
	u8 p[128], before[128], out;
	unsigned int dscp, inner, outer, len;
	int err;

	for (dscp = 0; dscp < 256; dscp += 4) {
		for (inner = 0; inner < 4; inner++) {
			for (outer = 0; outer < 4; outer++) {
				len = packet(p, IPPROTO_UDP, 3);
				p[1] = dscp | inner;
				repair_ip(p);
				memcpy(before, p, len);
				out = 0x7d;
				err = vpt1_ip_ecn_merge(p[1], 0x54 | outer, &out);
				if (table[inner][outer] < 0) {
					EXPECT(t, err < 0 && out == 0x7d);
					EXPECT(t, vpt1_ip_ecn_apply(p, len, outer) < 0);
					EXPECT(t, !memcmp(p, before, len));
					continue;
				}
				EXPECT(t, err == (!inner && outer ? 1 : 0));
				EXPECT(t, out == (dscp | table[inner][outer]));
				EXPECT(t, vpt1_ip_ecn_apply(p, len, outer) == err);
				EXPECT(t, p[1] == out && !checksum(p, 20, 0));
				EXPECT(t, p[8] == 63 && !memcmp(p + 12, before + 12, len - 12));
			}
		}
	}
	EXPECT(t, vpt1_ip_ecn_merge(0, 0, NULL) == -EINVAL);
}

static void outer_boundary(struct ip_test *t)
{
	u8 p[80];
	unsigned int len = packet(p, IPPROTO_UDP, 1), i;
	const u16 flags[] = { 0x2000, 1, 0x8000 };

	put_unaligned_be16(51888, p + 22);
	EXPECT(t, vpt1_ip_outer_guard(p, len, len) == 1);
	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		put_unaligned_be16(flags[i], p + 6);
		EXPECT(t, vpt1_ip_outer_guard(p, 20, len) < 0);
	}
	put_unaligned_be16(0x4000, p + 6);
	EXPECT(t, vpt1_ip_outer_guard(p, 20, len) < 0);
	EXPECT(t, vpt1_ip_outer_guard(p, len, len - 1) < 0);
	put_unaligned_be16(1501, p + 2);
	EXPECT(t, vpt1_ip_outer_guard(p, 28, 1501) < 0);
	put_unaligned_be16(1500, p + 2);
	EXPECT(t, vpt1_ip_outer_guard(p, 28, 1500) == 1);
	put_unaligned_be16(9090, p + 22);
	EXPECT(t, !vpt1_ip_outer_guard(p, 28, 1500));
	p[0] = 0x46;
	put_unaligned_be16(51888, p + 26);
	EXPECT(t, vpt1_ip_outer_guard(p, 32, 1500) < 0);
	p[9] = IPPROTO_TCP;
	EXPECT(t, !vpt1_ip_outer_guard(p, 32, 1500));
	EXPECT(t, vpt1_ip_outer_guard(NULL, 32, 1500) < 0);
}

static struct sk_buff *make_skb(const u8 *p, unsigned int len, bool nonlinear)
{
	struct sk_buff *skb = alloc_skb(64 + len, GFP_KERNEL);
	struct page *page;
	unsigned int head = nonlinear ? 24 : len;

	if (!skb)
		return NULL;
	skb_reserve(skb, 32);
	skb_reset_network_header(skb);
	skb->protocol = htons(ETH_P_IP);
	skb_put_data(skb, p, head);
	if (nonlinear) {
		page = alloc_page(GFP_KERNEL);
		if (!page) {
			kfree_skb(skb);
			return NULL;
		}
		memcpy(page_address(page), p + head, len - head);
		skb_add_rx_frag(skb, 0, page, 0, len - head, PAGE_SIZE);
	}
	return skb;
}

static void skb_storage_and_metadata(struct ip_test *t)
{
	u8 p[128];
	struct sk_buff *skb, *clone;
	struct vpt1_pkt_info out;
	unsigned int len = packet(p, IPPROTO_UDP, 31);

	skb = make_skb(p, len, true);
	EXPECT(t, skb);
	if (!skb)
		return;
	EXPECT(t, skb_is_nonlinear(skb));
	EXPECT(t, !vpt1_ip_skb_prepare(skb, &out));
	EXPECT(t, !skb_is_nonlinear(skb) && !memcmp(skb->data, p, len));
	kfree_skb(skb);
	skb = make_skb(p, len, false);
	EXPECT(t, skb);
	if (!skb)
		return;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->data[len - 1] ^= 1;
	EXPECT(t, vpt1_ip_skb_prepare(skb, &out) == -EBADMSG);
	skb->data[len - 1] ^= 1;
	skb_shinfo(skb)->gso_size = 8;
	EXPECT(t, vpt1_ip_skb_prepare(skb, &out) == -EOPNOTSUPP);
	skb_shinfo(skb)->gso_size = 0;
	skb->ip_summed = CHECKSUM_PARTIAL;
	skb->csum_start = skb_headroom(skb) + 20;
	skb->csum_offset = 5;
	EXPECT(t, vpt1_ip_skb_prepare(skb, &out) == -EPROTO);
	skb->csum_offset = 6;
	put_unaligned_be16(~checksum(NULL, 0, pseudo(p, len - 20)), skb->data + 26);
	clone = skb_clone(skb, GFP_KERNEL);
	EXPECT(t, clone);
	if (clone) {
		EXPECT(t, !vpt1_ip_skb_prepare(clone, &out));
		EXPECT(t, !checksum(clone->data + 20, len - 20, pseudo(p, len - 20)));
		EXPECT(t, memcmp(clone->data + 26, skb->data + 26, 2));
		kfree_skb(clone);
	}
	kfree_skb(skb);
	EXPECT(t, vpt1_ip_skb_prepare(NULL, &out) == -EINVAL);
}

static void partial_protocols(struct ip_test *t)
{
	const u8 protocols[] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };
	u8 p[128];
	struct sk_buff *skb;
	struct vpt1_pkt_info out;
	unsigned int i, nonlinear, len, offset;
	u32 seed;

	for (i = 0; i < ARRAY_SIZE(protocols); i++) {
		for (nonlinear = 0; nonlinear <= 1; nonlinear++) {
			len = packet(p, protocols[i], 17);
			offset = protocols[i] == IPPROTO_TCP ? 16 :
				 protocols[i] == IPPROTO_UDP ? 6 : 2;
			seed = protocols[i] == IPPROTO_ICMP ? 0 : pseudo(p, len - 20);
			put_unaligned_be16(~checksum(NULL, 0, seed), p + 20 + offset);
			skb = make_skb(p, len, nonlinear);
			EXPECT(t, skb);
			if (!skb)
				continue;
			skb->ip_summed = CHECKSUM_PARTIAL;
			skb->csum_start = skb_headroom(skb) + 20;
			skb->csum_offset = offset;
			EXPECT(t, !vpt1_ip_skb_prepare(skb, &out));
			EXPECT(t, !checksum(skb->data + 20, len - 20, seed));
			kfree_skb(skb);
		}
	}
}

static unsigned int error_packet(u8 *p, const u8 *quote, unsigned int quoted,
				 u8 type, u8 code)
{
	unsigned int len = packet(p, IPPROTO_ICMP, quoted);

	p[15] = 254; /* Router source need not be the peer. */
	p[19] = 1; /* The original packet's source. */
	p[20] = type;
	p[21] = code;
	memcpy(p + 28, quote, quoted);
	p[22] = 0;
	p[23] = 0;
	put_unaligned_be16(checksum(p + 20, len - 20, 0), p + 22);
	repair_ip(p);
	return len;
}

static void associated_error_quotes(struct ip_test *t)
{
	struct vpt1_recent_packet recent;
	u8 original[128], error[256];
	u64 now = 100ULL * NSEC_PER_SEC;
	unsigned int len, size, i;
	int expected;

	EXPECT(t, kmalloc_size_roundup(sizeof(recent)) <= 128);
	pr_info("VPT1_ERROR_LAYOUT: size=%zu charge=%zu prefix=%u\n", sizeof(recent),
		kmalloc_size_roundup(sizeof(recent)), VPT1_ERROR_PREFIX);
	len = packet(original, IPPROTO_UDP, 17);
	EXPECT(t, !vpt1_recent_save(&recent, VPT1_RECENT_INNER, 7, original, len, now));
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, ICMP_FRAG_NEEDED);
	EXPECT(t, !vpt1_recent_match(&recent, error, size, now));
	EXPECT(t, !vpt1_recent_match(&recent, error, size, now + VPT1_ERROR_RETENTION_NS - 1));
	EXPECT(t, vpt1_recent_match(&recent, error, size, now + VPT1_ERROR_RETENTION_NS) ==
		   -ESTALE);
	EXPECT(t, vpt1_recent_match(&recent, error, size, now - 1) == -ESTALE);
	EXPECT(t, recent.created_ns == now); /* No refresh from matching queries. */

	for (i = 0; i < len; i++) {
		size = error_packet(error, original, len, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL);
		error[28 + i] ^= 1;
		error[22] = 0;
		error[23] = 0;
		put_unaligned_be16(checksum(error + 20, size - 20, 0), error + 22);
		expected = i == 1 || i == 8 || i == 10 || i == 11 ? 0 : -ENOENT;
		EXPECT(t, vpt1_recent_match(&recent, error, size, now) == expected);
	}
	size = error_packet(error, original, 27, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH);
	EXPECT(t, vpt1_recent_match(&recent, error, size, now) == -EPROTO);
	for (i = 0; i < 256; i++) {
		size = error_packet(error, original, 28, i, 0);
		expected = i == ICMP_DEST_UNREACH || i == ICMP_TIME_EXCEEDED ? 0 :
			   -EPROTONOSUPPORT;
		EXPECT(t, vpt1_recent_match(&recent, error, size, now) == expected);
	}
	for (i = 0; i < 256; i++) {
		size = error_packet(error, original, 28, ICMP_DEST_UNREACH, i);
		EXPECT(t, vpt1_recent_match(&recent, error, size, now) ==
			   (i <= 15 ? 0 : -EPROTONOSUPPORT));
	}
	size = error_packet(error, original, 28, ICMP_TIME_EXCEEDED, 2);
	EXPECT(t, vpt1_recent_match(&recent, error, size, now) == -EPROTONOSUPPORT);
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, 0);
	error[size - 1] ^= 1;
	EXPECT(t, vpt1_recent_match(&recent, error, size, now) == -EBADMSG);
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, 0);
	error[19] = 2;
	repair_ip(error);
	EXPECT(t, vpt1_recent_match(&recent, error, size, now) == -ENOENT);

	EXPECT(t, !vpt1_recent_save(&recent, VPT1_RECENT_OUTER, 0, original, len, now));
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, ICMP_FRAG_NEEDED);
	EXPECT(t, vpt1_recent_match(&recent, error, size, now) == 1);
	/* An outer quotation is only a plausible path event, never an inner grant. */
	original[26] = 0;
	original[27] = 0;
	EXPECT(t, vpt1_recent_save(&recent, VPT1_RECENT_OUTER, 0, original, len, now) ==
		   -EPROTO);
	EXPECT(t, vpt1_recent_save(&recent, VPT1_RECENT_INNER, 0, original, len, now) ==
		   -EINVAL);
	EXPECT(t, recent.kind == VPT1_RECENT_OUTER && recent.created_ns == now);
}

/*
 * One fragment of a complete packet: header copied, length and fragment fields set, checksum
 * repaired.
 */
static unsigned int frag_of(u8 *out, const u8 *p, unsigned int offset, unsigned int size, bool more,
			    u16 extra_flags)
{
	memcpy(out, p, 20);
	memcpy(out + 20, p + 20 + offset, size);
	put_unaligned_be16(20 + size, out + 2);
	put_unaligned_be16((offset / 8) | (more ? IP_MF : 0) | extra_flags, out + 6);
	repair_ip(out);
	return 20 + size;
}

static void fragment_checks(struct ip_test *t)
{
	/* Module init is single threaded; keep them off the stack. */
	static u8 p[2048], f[2048], g[2048];
	unsigned int len = packet(p, IPPROTO_UDP, 1000), n;

	EXPECT(t, !vpt1_ip_is_fragment(p, len)); /* DF only. */
	EXPECT(t, vpt1_ip_fragment_check(p, len) == -EPROTO);
	n = frag_of(f, p, 0, 512, true, 0);
	EXPECT(t, vpt1_ip_is_fragment(f, n) && !vpt1_ip_fragment_check(f, n));
	n = frag_of(f, p, 512, 508, false, 0); /* The last fragment: offset only, any size. */
	EXPECT(t, vpt1_ip_is_fragment(f, n) && !vpt1_ip_fragment_check(f, n));
	n = frag_of(f, p, 0, 513, true, 0); /* MF payload not a multiple of eight. */
	EXPECT(t, vpt1_ip_fragment_check(f, n) == -EPROTO);
	n = frag_of(f, p, 0, 512, true, IP_DF); /* DF with MF is outside profile 1. */
	EXPECT(t, vpt1_ip_fragment_check(f, n) == -EPROTO);
	n = frag_of(f, p, 0, 512, true, IP_CE);
	EXPECT(t, vpt1_ip_fragment_check(f, n) == -EPROTO);
	n = frag_of(f, p, 0, 512, true, 0);
	memcpy(g, f, n);
	g[8] = 0; /* No lifetime left. */
	repair_ip(g);
	EXPECT(t, vpt1_ip_fragment_check(g, n) == -EPROTO);
	memcpy(g, f, n);
	g[0] = 0x46; /* Options. */
	repair_ip(g);
	EXPECT(t, vpt1_ip_fragment_check(g, n) == -EPROTO);
	memcpy(g, f, n);
	g[10] ^= 1; /* Header checksum. */
	EXPECT(t, vpt1_ip_fragment_check(g, n) == -EBADMSG);
	memcpy(g, f, n);
	g[9] = 47; /* GRE. */
	repair_ip(g);
	EXPECT(t, vpt1_ip_fragment_check(g, n) == -EPROTONOSUPPORT);
	g[9] = IPPROTO_ICMP;
	repair_ip(g);
	EXPECT(t, !vpt1_ip_fragment_check(g, n));
	g[9] = IPPROTO_TCP;
	repair_ip(g);
	EXPECT(t, !vpt1_ip_fragment_check(g, n));
	EXPECT(t, vpt1_ip_fragment_check(f, 20) == -EMSGSIZE); /* No payload. */
	EXPECT(t, vpt1_ip_fragment_check(f, n - 8) == -EPROTO); /* Length field mismatch. */
	EXPECT(t, vpt1_ip_fragment_check(NULL, n) == -EMSGSIZE);
	/* The extent may not pass the IPv4 maximum: offset 65520 plus even 8 bytes does. */
	memcpy(g, f, 28);
	put_unaligned_be16(28, g + 2);
	put_unaligned_be16(8190, g + 6);
	repair_ip(g);
	/* 65520 + 8 = 65528 > 65515: refused. */
	EXPECT(t, vpt1_ip_fragment_check(g, 28) == -EMSGSIZE);
}

static void fragment_ecn(struct ip_test *t)
{
	u8 p[256], f[256];
	unsigned int len = packet(p, IPPROTO_UDP, 200), n;

	p[1] = 0xa2; /* ECT(0) */
	repair_ip(p);
	n = frag_of(f, p, 0, 128, true, 0);
	EXPECT(t, !vpt1_ip_frag_ecn_apply(f, n, 0));
	EXPECT(t, (f[1] & 3) == 2 && !checksum(f, 20, 0));
	EXPECT(t, vpt1_ip_frag_ecn_apply(f, n, 3) == 0 && (f[1] & 3) == 3 && !checksum(f, 20, 0));
	EXPECT(t, f[1] >> 2 == 0xa2 >> 2); /* DSCP stays. */
	p[1] = 0xa0; /* Not-ECT */
	repair_ip(p);
	n = frag_of(f, p, 0, 128, true, 0);
	EXPECT(t, vpt1_ip_frag_ecn_apply(f, n, 3) == -EPROTO); /* CE on Not-ECT: drop. */
	EXPECT(t, vpt1_ip_frag_ecn_apply(f, n, 2) == 1 && (f[1] & 3) == 0); /* Anomaly, counted. */
	EXPECT(t, vpt1_ip_frag_ecn_apply(p, len, 0) == -EPROTO); /* Not a fragment. */
}

static void fragment_output(struct ip_test *t)
{
	static const unsigned int mtus[] = { 1208, 1280, 1428, 1500 };
	static const unsigned int sizes[] = { 29, 1207, 1208, 1209, 1280, 1281, 1428, 1429, 1500,
		1501,
					      2000, 4096, 9000, 32768, 65535 };
	u8 *p = kmalloc(65536, GFP_KERNEL), *r = kmalloc(65536, GFP_KERNEL),
		*f = kmalloc(2048, GFP_KERNEL);
	unsigned int m, s, i, count, length, covered, expect_count;
	u16 flags;
	int n;

	EXPECT(t, p && r && f);
	if (!p || !r || !f)
		goto out;
	for (m = 0; m < ARRAY_SIZE(mtus); m++) {
		for (s = 0; s < ARRAY_SIZE(sizes); s++) {
			length = sizes[s];
			packet(p, IPPROTO_UDP, length - 28);
			put_unaligned_be16(IP_DF * 0, p + 6); /* DF clear: fragmentable. */
			repair_ip(p);
			count = vpt1_ip_fragment_count(length, mtus[m]);
			expect_count = length <= mtus[m] ? 1 :
				       DIV_ROUND_UP(length - 20, (mtus[m] - 20) & ~7U);
			EXPECT(t, count == expect_count);
			memset(r, 0, 65536);
			covered = 0;
			for (i = 0; i < count; i++) {
				n = vpt1_ip_fragment_build(p, length, mtus[m], i, f);
				EXPECT(t, n >= 20 && (unsigned int)n <= mtus[m]);
				if (n < 20)
					break;
				if (count == 1) {
					EXPECT(t, n == length && !memcmp(f, p, length));
					covered = length - 20;
					break;
				}
				EXPECT(t, !checksum(f, 20, 0) && get_unaligned_be16(f + 2) == n);
				EXPECT(t, f[8] == p[8] && f[9] == p[9] &&
				       !memcmp(f + 12, p + 12, 8));
				flags = get_unaligned_be16(f + 6);
				EXPECT(t, ((flags & IP_MF) != 0) == (i + 1 < count));
				EXPECT(t, (flags & IP_OFFSET) * 8U == covered);
				EXPECT(t, i + 1 == count || !((n - 20) % 8));
				memcpy(r + covered, f + 20, n - 20);
				covered += n - 20;
			}
			EXPECT(t, covered == length - 20);
			EXPECT(t, count == 1 || !memcmp(r, p + 20, length - 20));
		}
	}
	/* DF is never cleared to hide a size failure, and misuse is refused. */
	packet(p, IPPROTO_UDP, 1472);
	EXPECT(t, p[6] & 0x40);
	EXPECT(t, vpt1_ip_fragment_count(1500, 1428) == 2);
	EXPECT(t, vpt1_ip_fragment_build(p, 1500, 1428, 0, f) == -EMSGSIZE);
	packet(p, IPPROTO_UDP, 1400);
	/* Fits: whole, DF kept. */
	EXPECT(t, vpt1_ip_fragment_build(p, 1428, 1428, 0, f) == 1428 && !memcmp(f, p, 1428));
	packet(p, IPPROTO_UDP, 1472);
	put_unaligned_be16(0, p + 6);
	repair_ip(p);
	EXPECT(t, vpt1_ip_fragment_build(p, 1500, 1428, 2, f) == -EINVAL);
	EXPECT(t, vpt1_ip_fragment_build(p, 1500, 20, 0, f) == -EINVAL); /* No room for payload. */
	put_unaligned_be16(IP_MF, p + 6);
	repair_ip(p);
	EXPECT(t, vpt1_ip_fragment_build(p, 1500, 1428, 0, f) == -EPROTO); /* Already a fragment. */
	EXPECT(t, vpt1_ip_fragment_count(19, 1428) == 0 &&
	       vpt1_ip_fragment_count(65536, 1428) == 0);
out:
	kfree(p);
	kfree(r);
	kfree(f);
}

static void error_packet_validation(struct ip_test *t)
{
	u8 original[128], error[256];
	struct vpt1_pkt_info pi = {}, saved;
	unsigned int len = packet(original, IPPROTO_UDP, 17), size, i;

	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH);
	EXPECT(t, vpt1_ip_is_icmp_error(error, size));
	EXPECT(t, !vpt1_ip_error_validate(error, size, &pi));
	EXPECT(t, pi.proto == IPPROTO_ICMP && pi.icmp_type == ICMP_DEST_UNREACH &&
	       pi.icmp_code == ICMP_PORT_UNREACH);
	EXPECT(t, pi.src == htonl(0x0a0000fe) && pi.dst == htonl(0x0a000001));
	size = error_packet(error, original, 28, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL);
	EXPECT(t, !vpt1_ip_error_validate(error, size, &pi) && pi.icmp_type == ICMP_TIME_EXCEEDED);
	/* Only these two families, with defined codes. */
	for (i = 0; i < 256; i++) {
		size = error_packet(error, original, 28, i, 0);
		EXPECT(t, vpt1_ip_is_icmp_error(error, size) ==
			   (i == ICMP_DEST_UNREACH || i == ICMP_TIME_EXCEEDED));
		EXPECT(t, (vpt1_ip_error_validate(error, size, &pi) == 0) ==
			   (i == ICMP_DEST_UNREACH || i == ICMP_TIME_EXCEEDED));
	}
	for (i = 0; i < 256; i++) {
		size = error_packet(error, original, 28, ICMP_DEST_UNREACH, i);
		EXPECT(t,
		       (vpt1_ip_error_validate(error, size, &pi) == 0) == (i <= NR_ICMP_UNREACH));
	}
	size = error_packet(error, original, 27, ICMP_DEST_UNREACH, 0);
	EXPECT(t, vpt1_ip_error_validate(error, size, &pi) == -EMSGSIZE); /* Short quotation. */
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, 0);
	error[size - 1] ^= 1;
	EXPECT(t, vpt1_ip_error_validate(error, size, &pi) == -EBADMSG);
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, 0);
	error[6] = 0x20; /* The error itself fragmented. */
	repair_ip(error);
	saved = pi;
	EXPECT(t, vpt1_ip_error_validate(error, size, &pi) == -EPROTO &&
	       !memcmp(&pi, &saved, sizeof(pi)));
	size = error_packet(error, original, 28, ICMP_DEST_UNREACH, 0);
	error[8] = 0;
	repair_ip(error);
	EXPECT(t, vpt1_ip_error_validate(error, size, &pi) == -EPROTO);
	EXPECT(t, vpt1_ip_error_validate(error, 577, &pi) == -EMSGSIZE);
	EXPECT(t, vpt1_ip_error_validate(NULL, size, &pi) == -EMSGSIZE);
	EXPECT(t, vpt1_ip_error_validate(error, size, NULL) == -EINVAL);
	EXPECT(t, !vpt1_ip_is_icmp_error(original, len)); /* An ordinary UDP packet. */
	size = packet(error, IPPROTO_ICMP, 8); /* Echo request. */
	EXPECT(t, !vpt1_ip_is_icmp_error(error, size));
}

static void skb_prelude(struct ip_test *t)
{
	static u8 p[2048], f[2048];
	unsigned int len = packet(p, IPPROTO_UDP, 1000), n = frag_of(f, p, 0, 512, true, 0);
	struct vpt1_pkt_info out;
	struct sk_buff *skb = alloc_skb(2048, GFP_KERNEL);

	EXPECT(t, skb);
	if (!skb)
		return;
	skb_put_data(skb, f, n);
	skb->protocol = htons(ETH_P_IP);
	skb_reset_network_header(skb);
	/*
	 * The prelude linearizes and completes checksums but leaves classification to the caller.
	 */
	EXPECT(t, !vpt1_ip_skb_linear(skb) && skb->len == n);
	/* A fragment is not an ordinary packet. */
	EXPECT(t, vpt1_ip_skb_prepare(skb, &out) == -EPROTO);
	skb_shinfo(skb)->gso_size = 100;
	EXPECT(t, vpt1_ip_skb_linear(skb) == -EOPNOTSUPP);
	skb_shinfo(skb)->gso_size = 0;
	skb->protocol = htons(ETH_P_IPV6);
	EXPECT(t, vpt1_ip_skb_linear(skb) == -EINVAL);
	EXPECT(t, vpt1_ip_skb_linear(NULL) == -EINVAL);
	kfree_skb(skb);
	(void)len;
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case cases[] = {
	KUNIT_CASE(lengths_and_caps), KUNIT_CASE(transport_checksums),
	KUNIT_CASE(ipv4_boundaries), KUNIT_CASE(ecn_table),
	KUNIT_CASE(outer_boundary), KUNIT_CASE(skb_storage_and_metadata),
	KUNIT_CASE(partial_protocols), KUNIT_CASE(associated_error_quotes),
	KUNIT_CASE(fragment_checks), KUNIT_CASE(fragment_ecn), KUNIT_CASE(fragment_output),
	KUNIT_CASE(error_packet_validation), KUNIT_CASE(skb_prelude), {}
};

static struct kunit_suite suite = { .name = "vpt1-ip", .test_cases = cases };
kunit_test_suite(suite);
#else
static int __init ip_test_init(void)
{
	struct ip_test t = {};

	lengths_and_caps(&t);
	transport_checksums(&t);
	ipv4_boundaries(&t);
	ecn_table(&t);
	outer_boundary(&t);
	skb_storage_and_metadata(&t);
	partial_protocols(&t);
	associated_error_quotes(&t);
	fragment_checks(&t);
	fragment_ecn(&t);
	fragment_output(&t);
	error_packet_validation(&t);
	skb_prelude(&t);
	pr_info("VPT1_IP_TEST: assertions=%u failures=%u %s\n", t.assertions,
		t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit ip_test_exit(void)
{
}

module_init(ip_test_init);
module_exit(ip_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 HR4 IPv4, checksum, ECN and skb boundaries");
