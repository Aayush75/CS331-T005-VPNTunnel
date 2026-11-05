"""Check the documented Netlink v1 identifiers and generated bindings."""

import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("netlink_ids_generator", ROOT / "tools/generate_netlink_ids.py")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class NetlinkIdTests(unittest.TestCase):
    def setUp(self):
        self.schema = json.loads((ROOT / "protocol/netlink-v1.json").read_text(encoding="utf-8"),
                                 object_pairs_hook=generator.reject_duplicate_keys)

    def test_command_ids_match_design_and_reserved_gap(self):
        expected = {
            "CLAIM": 1, "RENEW": 2, "VALIDATE": 3, "COMMIT_CONFIG": 4,
            "STAGE_GENERATION": 5, "EXCHANGE_TX": 6, "CONTROL_INTENT": 7,
            "DISABLE": 8, "REVOKE": 9, "STATUS": 10, "COUNTERS": 11,
            "REMOVE_GATEWAY": 12, "BEGIN_ATTEMPT": 13, "REKEY_REQUEST": 14,
            "EXCHANGE_RX": 128, "REKEY_GRANTED": 129, "REKEY_NEEDED": 130,
        }
        self.assertEqual({x["name"]: x["id"] for x in self.schema["commands"]}, expected)

    def test_attribute_ids_and_shapes_match_design(self):
        expected = [
            ("IFINDEX", 1, "u32", None), ("CONTROLLER_ID", 2, "bytes", 16),
            ("CONFIG_REVISION", 3, "u64", None), ("PEER_SLOT", 4, "u16", None),
            ("PEER_INCARNATION", 5, "bytes", 16), ("ATTEMPT_ID", 6, "bytes", 16),
            ("HANDSHAKE_HASH", 7, "bytes", 32), ("LOCAL_RX", 8, "u64", None),
            ("REMOTE_RX", 9, "u64", None), ("TX_KEY", 10, "bytes", 32),
            ("RX_KEY", 11, "bytes", 32), ("MODE", 12, "u8", None),
            ("PARENT_HASH", 13, "bytes", 32), ("EXCHANGE_BYTES", 14, "binary", 224),
            ("SNAPSHOT", 15, "nested", 61440), ("INTENT", 16, "u8", None),
            ("STATUS", 17, "nested", None), ("COUNTERS", 18, "nested", None),
            ("REQUEST_ID", 19, "u64", None),
        ]
        actual = [(a["name"], a["id"], a["type"], a.get("length", a.get("max_length")))
                  for a in self.schema["attributes"]]
        self.assertEqual(actual, expected)
        self.assertEqual(self.schema["max_message_bytes"], 65536)

    def test_nested_attributes_are_generated_as_nested_only(self):
        nested = {a["name"] for a in self.schema["attributes"] if a["type"] == "nested"}
        self.assertEqual(nested, {"SNAPSHOT", "STATUS", "COUNTERS"})
        header, rust = generator.render(self.schema)
        for name in nested:
            self.assertIn(f"#define VPT1_ATTR_{name}_NESTED 1", header)
        self.assertIn("matches!(id, ATTR_SNAPSHOT | ATTR_STATUS | ATTR_COUNTERS)", rust)

    def test_bindings_are_current(self):
        for path, expected in zip((generator.HEADER, generator.RUST), generator.render(self.schema)):
            self.assertEqual(path.read_text(encoding="utf-8"), expected, str(path))

    def test_invalid_schema_is_rejected(self):
        for mutate in (
            lambda s: s["commands"].append(copy.deepcopy(s["commands"][0])),
            lambda s: s["attributes"][0].update(id=0),
            lambda s: s["attributes"][0].update(type="pointer"),
            lambda s: s["attributes"][1].update(length=0),
            lambda s: s["attributes"][0].update(extra=True),
        ):
            schema = copy.deepcopy(self.schema)
            mutate(schema)
            with self.subTest(schema=schema), self.assertRaises(ValueError):
                generator.validate(schema)
        with self.assertRaises(ValueError):
            json.loads('{"id": 1, "id": 2}', object_pairs_hook=generator.reject_duplicate_keys)


if __name__ == "__main__":
    unittest.main()
