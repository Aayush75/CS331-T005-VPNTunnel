/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VPT1_WIRE_H
#define VPT1_WIRE_H

#include <linux/types.h>

#define VPT1_ENVELOPE_LEN 16U
#define VPT1_AAD_LEN 24U
#define VPT1_TAG_LEN 16U
#define VPT1_PREFIX_LEN 4U
#define VPT1_NONCE_LEN 12U
#define VPT1_ATTEMPT_LEN 16U
#define VPT1_INIT_LEN 224U
#define VPT1_RESPONSE_LEN 176U
#define VPT1_INNER_CAP_MIN 1208U
#define VPT1_INNER_CAP_MAX 1428U
#define VPT1_RECORD_EXPANSION 44U /* UDP payload length minus inner length */
#define VPT1_COUNTER_LIMIT 1048576ULL

enum vpt1_kind {
	VPT1_INIT = 1,
	VPT1_RESPONSE = 2,
	VPT1_RECORD = 3,
};

enum vpt1_subtype {
	VPT1_IPV4 = 1,
	VPT1_CONFIRM = 2,
	VPT1_READY = 3,
	VPT1_COMMIT = 4,
	VPT1_COMMITTED = 5,
	VPT1_PING = 6,
	VPT1_PONG = 7,
	VPT1_REKEY_START = 8,
	VPT1_REKEY_OK = 9,
	VPT1_REKEY_NEEDED = 10,
};

/* Borrowed views, host-order integers. Never copy these structures to the wire. */
struct vpt1_datagram {
	u64 locator;
	u64 counter; /* RECORD only */
	const u8 *body;
	const u8 *attempt; /* INIT/RESPONSE only; clear and unauthenticated */
	const u8 *noise;
	const u8 *aad; /* RECORD: original first 24 bytes, no reserialization */
	const u8 *ciphertext;
	const u8 *tag;
	u16 body_len;
	u16 noise_len;
	u16 ciphertext_len;
	u8 kind;
};

struct vpt1_plaintext {
	const u8 *payload;
	u16 payload_len;
	u8 subtype;
};

/*
 * Pure bounded parsers. Borrow input; no allocation, mutation or state updates.
 * Input must be contiguous and remain alive/unchanged while a view is in use.
 * Supports unaligned input; caller must handle nonlinear skbs before calling.
 * inner_cap is this authenticated/configured peer's RX cap, not device TX MTU.
 * Output is assigned only on success (0), unchanged on any negative errno.
 * -EINVAL: caller arguments/cap; -EMSGSIZE: length; -EPROTO: format;
 * -ERANGE: record counter outside the selected profile.
 */
int vpt1_parse_datagram(const u8 *data, size_t len, u16 inner_cap,
			struct vpt1_datagram *out);

/* Call only AFTER AEAD success and replay claim; does not authenticate bytes. */
int vpt1_parse_authenticated_plaintext(const u8 *data, size_t len,
				       u16 inner_cap,
				       struct vpt1_plaintext *out);

/* Encoding only; reservation/range checking belongs to the caller. */
void vpt1_counter_nonce(u64 counter, u8 nonce[VPT1_NONCE_LEN]);

#endif /* VPT1_WIRE_H */
