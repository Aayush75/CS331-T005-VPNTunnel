#!/usr/bin/env python3
"""Independent, test-only Noise IK/VPT1 vector generator.

Uses cryptography's X25519 and ChaCha20-Poly1305 primitives and Python's
BLAKE2s/HMAC. All private keys below are public deterministic test values.
Never use this script or its keys for a real session.
"""

import hashlib
import hmac
import json
from pathlib import Path
import struct

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = b"Noise_IK_25519_ChaChaPoly_BLAKE2s"


def digest(value):
    return hashlib.blake2s(value).digest()


def mac(key, value):
    return hmac.new(key, value, hashlib.blake2s).digest()


def hkdf2(chaining_key, material):
    temporary = mac(chaining_key, material)
    first = mac(temporary, b"\x01")
    return first, mac(temporary, first + b"\x02")


def private(raw):
    return X25519PrivateKey.from_private_bytes(raw)


def public(key):
    return key.public_key().public_bytes(serialization.Encoding.Raw,
                                         serialization.PublicFormat.Raw)


def encrypt(key, nonce, plaintext, aad):
    return ChaCha20Poly1305(key).encrypt(b"\x00" * 4 + nonce.to_bytes(8, "little"), plaintext, aad)


def ik(init_static, init_ephemeral, resp_static, resp_ephemeral, prologue, first, second):
    """Construct both IK messages and raw Split independently of snow."""
    i_s, i_e, r_s, r_e = map(private, (init_static, init_ephemeral, resp_static, resp_ephemeral))
    h = PROTOCOL.ljust(32, b"\x00") if len(PROTOCOL) <= 32 else digest(PROTOCOL)
    ck = h
    h = digest(h + prologue)
    h = digest(h + public(r_s))  # IK responder static pre-message.
    message1 = public(i_e)
    h = digest(h + message1)
    ck, key = hkdf2(ck, i_e.exchange(r_s.public_key()))  # es
    part = encrypt(key, 0, public(i_s), h)
    message1 += part
    h = digest(h + part)
    ck, key = hkdf2(ck, i_s.exchange(r_s.public_key()))  # ss
    part = encrypt(key, 0, first, h)
    message1 += part
    h = digest(h + part)
    message2 = public(r_e)
    h = digest(h + message2)
    ck, _ = hkdf2(ck, i_e.exchange(r_e.public_key()))  # ee
    ck, key = hkdf2(ck, i_s.exchange(r_e.public_key()))  # se
    part = encrypt(key, 0, second, h)
    message2 += part
    h = digest(h + part)
    return message1, message2, h, hkdf2(ck, b"")


def verify_upstream():
    entry = json.loads((ROOT / "control/tests/vectors/cacophony-ik-25519-chachapoly-blake2s.json").read_text())[
        "vector"]
    b = lambda key: bytes.fromhex(entry[key])  # noqa: E731
    messages = entry["messages"]
    m1, m2, transcript, keys = ik(b("init_static"), b("init_ephemeral"), b("resp_static"),
                                    b("resp_ephemeral"), b("init_prologue"),
                                    bytes.fromhex(messages[0]["payload"]),
                                    bytes.fromhex(messages[1]["payload"]))
    checks = (m1.hex() == messages[0]["ciphertext"],
              m2.hex() == messages[1]["ciphertext"],
              transcript.hex() == entry["handshake_hash"],
              encrypt(keys[0], 0, bytes.fromhex(messages[2]["payload"]), b"").hex()
              == messages[2]["ciphertext"])
    if not all(checks):
        raise ValueError("independent Noise oracle disagrees with upstream IK vector")


def payload(attempt, incarnation, initiator_locator, responder_locator):
    return (b"\x00" * 8 + attempt + incarnation + struct.pack(">QQ", initiator_locator, responder_locator)
            + b"\x00" * 32 + struct.pack(">IHH", 1, 1500, 1428))


def envelope(kind, locator, attempt, noise):
    return b"VPT1" + bytes((kind, 0)) + struct.pack(">HQ", len(attempt) + len(noise), locator) + attempt + noise


def main():
    verify_upstream()
    i_s, i_e, r_s, r_e = (bytes([value]) * 32 for value in (1, 2, 3, 4))
    attempt = bytes.fromhex("11" * 16)
    initiator_locator, responder_locator = 0x1111222233334444, 0x5555666677778888
    first = payload(attempt, bytes.fromhex("a1" * 16), initiator_locator, 0)
    second = payload(attempt, bytes.fromhex("b2" * 16), initiator_locator, responder_locator)
    prologue = b"VPNTUNNEL-V1" + b"\x00" * 4 + public(private(i_s)) + public(private(r_s))
    m1, m2, transcript, (k1, k2) = ik(i_s, i_e, r_s, r_e, prologue, first, second)
    values = {"init_static": i_s, "init_ephemeral": i_e, "resp_static": r_s, "resp_ephemeral": r_e,
              "init_public": public(private(i_s)), "resp_public": public(private(r_s)),
              "prologue": prologue, "init_payload": first, "response_payload": second,
              "init": envelope(1, 0, attempt, m1),
              "response": envelope(2, initiator_locator, attempt, m2),
              "handshake_hash": transcript, "initiator_tx": k1, "initiator_rx": k2}
    print(json.dumps({"notice": "Public deterministic test keys; never use in a real session.",
                      "oracle": "Python cryptography X25519/ChaCha20Poly1305 and hashlib BLAKE2s; verified against upstream cacophony IK vector",
                      **{key: value.hex() for key, value in values.items()}}, indent=2))


if __name__ == "__main__":
    main()
