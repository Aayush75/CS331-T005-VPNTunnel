#!/usr/bin/env python3
"""Generate Netlink v1 identifier bindings; never parse or authorize requests."""

import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "protocol/netlink-v1.json"
HEADER = ROOT / "kernel/include/vpt1_netlink_ids.h"
RUST = ROOT / "control/src/netlink_ids.rs"
NAME = re.compile(r"[A-Z][A-Z0-9_]*\Z")


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def validate(schema):
    if set(schema) != {"family", "version", "max_message_bytes", "commands", "attributes"}:
        raise ValueError("unknown or missing schema field")
    if schema.get("family") != "vptunnel" or type(schema.get("version")) is not int or schema["version"] != 1:
        raise ValueError("unexpected family or ABI version")
    if schema.get("max_message_bytes") != 65536:
        raise ValueError("unexpected message bound")
    for group, limit in (("commands", 255), ("attributes", 16383)):
        names, ids = set(), set()
        for item in schema[group]:
            allowed = {"name", "id"} if group == "commands" else {
                "name", "id", "type", "length", "max_length"}
            if not set(item) <= allowed or not {"name", "id"} <= set(item):
                raise ValueError(f"unknown or missing {group} field")
            name, number = item["name"], item["id"]
            if not isinstance(name, str) or not NAME.fullmatch(name):
                raise ValueError(f"invalid {group} name")
            if type(number) is not int or not 1 <= number <= limit:
                raise ValueError(f"invalid {group} identifier")
            if name in names or number in ids:
                raise ValueError(f"duplicate {group} identifier or name")
            names.add(name)
            ids.add(number)
            if group == "attributes":
                kind = item.get("type")
                if kind not in {"u8", "u16", "u32", "u64", "bytes", "binary", "nested"}:
                    raise ValueError("invalid attribute type")
                if kind == "bytes" and (type(item.get("length")) is not int or
                                        not 1 <= item["length"] <= 65536):
                    raise ValueError("invalid fixed byte length")
                if "length" in item and kind != "bytes":
                    raise ValueError("fixed length is only valid for bytes")
                if "max_length" in item and (kind not in {"binary", "nested"} or
                                             type(item["max_length"]) is not int or
                                             not 1 <= item["max_length"] <= 65536):
                    raise ValueError("invalid maximum length")


def render(schema):
    validate(schema)
    c = ["/* SPDX-License-Identifier: GPL-2.0-only */",
         "/* Generated from protocol/netlink-v1.json; do not edit. */",
         "#ifndef VPT1_NETLINK_IDS_H", "#define VPT1_NETLINK_IDS_H", "",
         '#define VPT1_GENL_FAMILY "vptunnel"', "#define VPT1_GENL_VERSION 1",
         "#define VPT1_NETLINK_MAX_MESSAGE 65536", ""]
    rust = ["//! Generated from `protocol/netlink-v1.json`; do not edit.",
            "//! Identifiers only: this module does not validate or send Netlink messages.", "",
            'pub const FAMILY: &str = "vptunnel";', "pub const VERSION: u8 = 1;",
            "pub const MAX_MESSAGE_BYTES: usize = 65536;", ""]
    for group, prefix in (("commands", "CMD"), ("attributes", "ATTR")):
        for item in schema[group]:
            c.append(f'#define VPT1_{prefix}_{item["name"]} {item["id"]}')
            number_type = "u8" if group == "commands" else "u16"
            rust.append(f'pub const {prefix}_{item["name"]}: {number_type} = {item["id"]};')
            for key, suffix in (("length", "LEN"), ("max_length", "MAX_LEN")):
                if key in item:
                    c.append(f'#define VPT1_{prefix}_{item["name"]}_{suffix} {item[key]}')
                    rust.append(f'pub const {prefix}_{item["name"]}_{suffix}: usize = {item[key]};')
        c.append("")
        rust.append("")
    # Nested attributes must carry NLA_F_NESTED; all others must not.
    nested = [item["name"] for item in schema["attributes"] if item["type"] == "nested"]
    for name in nested:
        c.append(f"#define VPT1_ATTR_{name}_NESTED 1")
    c.append("")
    alternatives = " | ".join(f"ATTR_{name}" for name in nested)
    rust.extend(["/// True for attributes that must carry `NLA_F_NESTED` (and only those).",
                 "pub const fn attr_is_nested(id: u16) -> bool {",
                 f"    matches!(id, {alternatives})",
                 "}", ""])
    c.extend(["#endif /* VPT1_NETLINK_IDS_H */", ""])
    return "\n".join(c), "\n".join(rust)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if checked-in bindings are stale")
    args = parser.parse_args()
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicate_keys)
    for path, content in zip((HEADER, RUST), render(schema)):
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                parser.exit(1, f"stale generated file: {path.relative_to(ROOT)}\n")
        else:
            path.write_text(content, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
