#!/usr/bin/env python3
"""Check public AEAD fixtures through system OpenSSL, independent of kernel code."""
import argparse
import ctypes as C
from ctypes.util import find_library
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tests/conformance/vpt1_aead_vectors.json'
HEADER = ROOT / 'tests/conformance/vpt1_aead_vectors.h'


def encrypt(key, nonce, aad, plaintext):
    lib = C.CDLL(find_library('crypto'))
    ptr = C.c_void_p
    signatures = {
        'EVP_CIPHER_CTX_new': (ptr, []),
        'EVP_CIPHER_CTX_free': (None, [ptr]),
        'EVP_chacha20_poly1305': (ptr, []),
        'EVP_EncryptInit_ex': (C.c_int, [ptr, ptr, ptr, ptr, ptr]),
        'EVP_CIPHER_CTX_ctrl': (C.c_int, [ptr, C.c_int, C.c_int, ptr]),
        'EVP_EncryptUpdate': (C.c_int, [ptr, ptr, ptr, ptr, C.c_int]),
        'EVP_EncryptFinal_ex': (C.c_int, [ptr, ptr, ptr]),
    }
    for name, (result, arguments) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, arguments
    ctx = lib.EVP_CIPHER_CTX_new()
    if not ctx:
        raise RuntimeError('EVP context allocation failed')
    try:
        output, tag = C.create_string_buffer(len(plaintext) + 16), C.create_string_buffer(16)
        length = C.c_int()
        operations = [
            lib.EVP_EncryptInit_ex(ctx, lib.EVP_chacha20_poly1305(), None, None, None),
            lib.EVP_CIPHER_CTX_ctrl(ctx, 0x9, len(nonce), None),  # EVP_CTRL_AEAD_SET_IVLEN
            lib.EVP_EncryptInit_ex(ctx, None, None, C.c_char_p(key), C.c_char_p(nonce)),
            lib.EVP_EncryptUpdate(ctx, None, C.byref(length), C.c_char_p(aad), len(aad)),
            lib.EVP_EncryptUpdate(ctx, output, C.byref(length),
                                 C.c_char_p(plaintext), len(plaintext)),
        ]
        produced = length.value
        operations.append(lib.EVP_EncryptFinal_ex(ctx, C.byref(output, produced),
                                                 C.byref(length)))
        produced += length.value
        operations.append(lib.EVP_CIPHER_CTX_ctrl(ctx, 0x10, 16, tag))  # GET_TAG
        if any(value != 1 for value in operations):
            raise RuntimeError('OpenSSL AEAD operation failed')
        return output.raw[:produced] + tag.raw[:16]
    finally:
        lib.EVP_CIPHER_CTX_free(ctx)


def render(vectors):
    lines = ['/* SPDX-License-Identifier: GPL-2.0-only */',
             '/* Generated from public JSON fixtures by tools/ay3_aead_vectors.py. */']
    for i, vector in enumerate(vectors):
        for field in ('key', 'nonce', 'aad', 'plaintext', 'protected'):
            data = bytes.fromhex(vector[field])
            lines.append(f'static const u8 aead_{i}_{field}[] = {{')
            for offset in range(0, len(data), 12):
                lines.append('\t' + ', '.join(f'0x{b:02x}' for b in data[offset:offset+12]) + ',')
            lines.extend(['};', ''])
    lines.append('static const struct vpt1_aead_vector aead_vectors[] = {')
    for i, vector in enumerate(vectors):
        lines.extend(['\t{', f'\t\t.name = "{vector["name"]}",'])
        for field in ('key', 'nonce', 'aad', 'plaintext', 'protected'):
            lines.append(f'\t\t.{field} = aead_{i}_{field},')
        lines.extend([f'\t\t.aad_len = sizeof(aead_{i}_aad),',
                      f'\t\t.plaintext_len = sizeof(aead_{i}_plaintext),', '\t},'])
    lines.append('};')
    return '\n'.join(lines).rstrip() + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    vectors = json.loads(SOURCE.read_text())['vectors']
    for vector in vectors:
        output = encrypt(*(bytes.fromhex(vector[f]) for f in
                           ('key', 'nonce', 'aad', 'plaintext')))
        if output.hex() != vector['protected']:
            raise SystemExit('FAIL: independent AEAD mismatch: ' + vector['name'])
    header = render(vectors)
    if args.check:
        if HEADER.read_text() != header:
            raise SystemExit('FAIL: regenerate AEAD fixture header')
    else:
        HEADER.write_text(header)
    print(f'PASS: OpenSSL matches all {len(vectors)} public AEAD fixtures')


if __name__ == '__main__':
    main()
