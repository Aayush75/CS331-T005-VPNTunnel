// SPDX-License-Identifier: GPL-2.0-only
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "vpt1_wire.h"

static_assert(VPT1_ENVELOPE_LEN + sizeof(u64) == VPT1_AAD_LEN);
static_assert(VPT1_AAD_LEN + VPT1_PREFIX_LEN + VPT1_TAG_LEN == VPT1_RECORD_EXPANSION);
static_assert(VPT1_ENVELOPE_LEN + VPT1_ATTEMPT_LEN + 192 == VPT1_INIT_LEN);
static_assert(VPT1_ENVELOPE_LEN + VPT1_ATTEMPT_LEN + 144 == VPT1_RESPONSE_LEN);

static bool vpt1_valid_cap(u16 cap)
{
	return cap >= VPT1_INNER_CAP_MIN && cap <= VPT1_INNER_CAP_MAX;
}

int vpt1_parse_datagram(const u8 *data, size_t len, u16 inner_cap,
			struct vpt1_datagram *out)
{
	struct vpt1_datagram view = {};

	if (!data || !out || !vpt1_valid_cap(inner_cap))
		return -EINVAL;
	if (len < VPT1_ENVELOPE_LEN ||
	    len > inner_cap + VPT1_RECORD_EXPANSION)
		return -EMSGSIZE;
	if (memcmp(data, "VPT1", 4) || data[5])
		return -EPROTO;
	view.kind = data[4];
	view.body_len = get_unaligned_be16(data + 6);
	if (view.body_len != len - VPT1_ENVELOPE_LEN)
		return -EMSGSIZE;
	view.locator = get_unaligned_be64(data + 8);
	view.body = data + VPT1_ENVELOPE_LEN;

	switch (view.kind) {
	case VPT1_INIT:
		if (view.locator)
			return -EPROTO;
		if (len != VPT1_INIT_LEN)
			return -EMSGSIZE;
		break;
	case VPT1_RESPONSE:
		if (!view.locator)
			return -EPROTO;
		if (len != VPT1_RESPONSE_LEN)
			return -EMSGSIZE;
		break;
	case VPT1_RECORD:
		if (!view.locator)
			return -EPROTO;
		if (view.body_len < sizeof(u64) + VPT1_PREFIX_LEN + VPT1_TAG_LEN)
			return -EMSGSIZE;
		view.counter = get_unaligned_be64(view.body);
		if (view.counter >= VPT1_COUNTER_LIMIT)
			return -ERANGE;
		view.aad = data;
		view.ciphertext = data + VPT1_AAD_LEN;
		view.ciphertext_len = len - VPT1_AAD_LEN - VPT1_TAG_LEN;
		view.tag = data + len - VPT1_TAG_LEN;
		*out = view;
		return 0;
	default:
		return -EPROTO;
	}

	view.attempt = view.body;
	view.noise = view.body + VPT1_ATTEMPT_LEN;
	view.noise_len = view.body_len - VPT1_ATTEMPT_LEN;
	*out = view;
	return 0;
}

int vpt1_parse_authenticated_plaintext(const u8 *data, size_t len,
				       u16 inner_cap,
				       struct vpt1_plaintext *out)
{
	struct vpt1_plaintext view = {};
	u16 expected;

	if (!data || !out || !vpt1_valid_cap(inner_cap))
		return -EINVAL;
	if (len < VPT1_PREFIX_LEN || len > inner_cap + VPT1_PREFIX_LEN)
		return -EMSGSIZE;
	if (data[1])
		return -EPROTO;
	view.subtype = data[0];
	view.payload_len = get_unaligned_be16(data + 2);
	if (view.payload_len != len - VPT1_PREFIX_LEN)
		return -EMSGSIZE;
	view.payload = data + VPT1_PREFIX_LEN;

	switch (view.subtype) {
	case VPT1_IPV4:
		if (view.payload_len < 20)
			return -EMSGSIZE;
		/* Profile 1 excludes IPv4 options. Checksums/policy are separate. */
		if (view.payload[0] != 0x45)
			return -EPROTO;
		if (get_unaligned_be16(view.payload + 2) != view.payload_len)
			return -EMSGSIZE;
		*out = view;
		return 0;
	case VPT1_CONFIRM:
	case VPT1_READY:
	case VPT1_COMMIT:
	case VPT1_COMMITTED:
	case VPT1_REKEY_NEEDED:
		expected = 32;
		break;
	case VPT1_PING:
	case VPT1_PONG:
		expected = 16;
		break;
	case VPT1_REKEY_START:
	case VPT1_REKEY_OK:
		expected = 48;
		break;
	default:
		return -EPROTO;
	}
	if (view.payload_len != expected)
		return -EMSGSIZE;
	*out = view;
	return 0;
}

void vpt1_counter_nonce(u64 counter, u8 nonce[VPT1_NONCE_LEN])
{
	memset(nonce, 0, 4);
	put_unaligned_le64(counter, nonce + 4);
}
