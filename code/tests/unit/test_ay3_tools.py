"""Ensure the independent fixtures and guest result classifier detect failures."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vectors = load('ay3_vectors')
runner = load('ay3_qemu_test')
aead = load('ay3_aead_vectors')


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.fixtures = json.loads(vectors.SOURCE.read_text())['vectors']

    def test_golden_provenance_and_generated_header(self):
        for fixture in self.fixtures:
            self.assertEqual(vectors.inspect(fixture), fixture['expected'])
        self.assertEqual(vectors.HEADER.read_text(), vectors.render(self.fixtures))

    def test_corrupt_fields_detected_by_independent_oracle(self):
        for fixture in self.fixtures:
            wire = bytes.fromhex(fixture['hex'])
            for changed in (wire[:-1], wire + b'\x00'):
                bad = copy.deepcopy(fixture)
                bad['hex'] = changed.hex()
                with self.assertRaises((AssertionError, ValueError)):
                    vectors.inspect(bad)
            bad = copy.deepcopy(fixture)
            flags_offset = 1 if fixture['stage'] == 'plaintext' else 5
            wire = bytearray(wire)
            wire[flags_offset] = 1
            bad['hex'] = wire.hex()
            with self.assertRaises(AssertionError):
                vectors.inspect(bad)


class AeadVectorTests(unittest.TestCase):
    def test_reference_ciphertext_and_generated_header(self):
        fixtures = json.loads(aead.SOURCE.read_text())['vectors']
        self.assertEqual(aead.HEADER.read_text(), aead.render(fixtures))
        for fixture in fixtures:
            protected = aead.encrypt(*(bytes.fromhex(fixture[field]) for field in
                                       ('key', 'nonce', 'aad', 'plaintext')))
            self.assertEqual(protected.hex(), fixture['protected'])

    def test_reference_binds_aad_and_nonce(self):
        fixture = json.loads(aead.SOURCE.read_text())['vectors'][1]
        key, nonce, aad_bytes, plaintext = (bytes.fromhex(fixture[f]) for f in
                                            ('key', 'nonce', 'aad', 'plaintext'))
        self.assertNotEqual(aead.encrypt(key, nonce, bytes([aad_bytes[0] ^ 1]) +
                                        aad_bytes[1:], plaintext).hex(), fixture['protected'])
        self.assertNotEqual(aead.encrypt(key, bytes([nonce[0] ^ 1]) + nonce[1:],
                                        aad_bytes, plaintext).hex(), fixture['protected'])


class ResultTests(unittest.TestCase):
    VALID = 'VPT1_PARSER_TEST: assertions=12000 failures=0 PASS\nVPT1_PARSER_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n'

    def test_success(self):
        self.assertTrue(runner.assess(self.VALID, 0))

    def test_sequence_suite_is_separate(self):
        sequence = self.VALID.replace('PARSER', 'SEQUENCE')
        self.assertTrue(runner.assess(sequence, 0, 'sequence'))
        self.assertFalse(runner.assess(self.VALID, 0, 'sequence'))
        self.assertFalse(runner.assess(sequence, 0, 'parser'))
        crypto = self.VALID.replace('PARSER', 'AEAD')
        self.assertTrue(runner.assess(crypto, 0, 'aead'))
        self.assertFalse(runner.assess(sequence, 0, 'aead'))

    def test_failed_missing_or_partial_suite(self):
        for output in ('', 'VPT1_PARSER_GUEST_PASS', self.VALID.replace('12000', '1'),
                       self.VALID.replace('failures=0', 'failures=1'),
                       self.VALID.replace('VPT1_PARSER_GUEST_PASS', ''),
                       self.VALID.replace('CPUS=0-1', 'CPUS=0')):
            self.assertFalse(runner.assess(output, 0))
        self.assertFalse(runner.assess(self.VALID, 1))

    def test_kunit_suite_and_failures(self):
        output = ('[ 1.0] # Subtest: vpt1-sequence\n'
                  '[ 1.1] ok 1 - vpt1-sequence\nVPT1_SEQUENCE_GUEST_PASS\n'
                  '# vpt1-sequence: pass:5 fail:0 skip:0 total:5\n'
                  'VPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'sequence', 'kunit'))
        for addition in ('not ok 1 - replay', 'ok 1 - replay # SKIP', 'UBSAN: shift',
                         'possible circular locking dependency'):
            self.assertFalse(runner.assess(output + addition, 0, 'sequence', 'kunit'))
        self.assertFalse(runner.assess(output.replace('ok 1', 'not ok 1'), 0,
                                       'sequence', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'parser', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'sequence', 'standalone'))

    def test_kernel_faults_and_setup_failure(self):
        for error in ('BUG:', 'WARNING:', 'Oops:', 'Kernel panic',
                      'refcount_t:', 'GUEST_ERROR'):
            self.assertFalse(runner.assess(self.VALID + error, 0))

    def test_acl_requires_its_own_complete_suite(self):
        standalone = self.VALID.replace('PARSER', 'ACL')
        self.assertTrue(runner.assess(standalone, 0, 'acl'))
        self.assertFalse(runner.assess(self.VALID, 0, 'acl'))
        output = ('# Subtest: vpt1-acl\nok 1 - vpt1-acl\n'
                  '# vpt1-acl: pass:6 fail:0 skip:0 total:6\n'
                  'VPT1_ACL_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'acl', 'kunit'))
        for changed in (output.replace('pass:6', 'pass:5'),
                        output.replace('skip:0', 'skip:1'),
                        output.replace('VPT1_ACL_GUEST_PASS', ''),
                        output + 'BUG: KASAN: out-of-bounds',
                        output + 'not ok 1 - policy'):
            self.assertFalse(runner.assess(changed, 0, 'acl', 'kunit'))

    def test_flow_requires_complete_own_suite(self):
        output = ('# Subtest: vpt1-flow\nok 1 - vpt1-flow\n'
                  '# vpt1-flow: pass:5 fail:0 skip:0 total:5\n'
                  'VPT1_FLOW_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'flow', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'acl', 'kunit'))
        for changed in (output.replace('pass:5', 'pass:4'),
                        output.replace('skip:0', 'skip:1'), output + 'UBSAN: fault'):
            self.assertFalse(runner.assess(changed, 0, 'flow', 'kunit'))

    def test_zone_requires_complete_own_suite(self):
        output = ('# Subtest: vpt1-zone\nok 1 - vpt1-zone\n'
                  '# vpt1-zone: pass:4 fail:0 skip:0 total:4\n'
                  'VPT1_ZONE_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'zone', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'acl', 'kunit'))
        for changed in (output.replace('pass:4', 'pass:3'),
                        output.replace('skip:0', 'skip:1'), output + 'BUG: fault'):
            self.assertFalse(runner.assess(changed, 0, 'zone', 'kunit'))

    def test_ip_requires_complete_own_suite(self):
        output = ('# Subtest: vpt1-ip\nok 1 - vpt1-ip\n'
                  '# vpt1-ip: pass:8 fail:0 skip:0 total:8\n'
                  'VPT1_IP_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'ip', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'zone', 'kunit'))
        for changed in (output.replace('pass:8', 'pass:7'),
                        output.replace('skip:0', 'skip:1'), output + 'BUG: fault',
                        output.replace('VPT1_IP_GUEST_PASS', '')):
            self.assertFalse(runner.assess(changed, 0, 'ip', 'kunit'))
        standalone = self.VALID.replace('PARSER', 'IP')
        self.assertTrue(runner.assess(standalone, 0, 'ip'))
        self.assertFalse(runner.assess(standalone.replace('12000', '1'), 0, 'ip'))


    def test_fragment_suite_requires_its_complete_clean_result(self):
        output = ('# Subtest: vpt1-fragment\nok 1 - vpt1-fragment\n'
                  '# vpt1-fragment: pass:14 fail:0 skip:0 total:14\n'
                  'VPT1_FRAGMENT_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'fragment', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'parser', 'kunit'))
        for changed in (output.replace('pass:14', 'pass:13'),
                        output.replace('skip:0', 'skip:1'),
                        output.replace('CPUS=0-1', 'CPUS=0'),
                        output.replace('VPT1_FRAGMENT_GUEST_PASS', ''),
                        output + 'BUG: KASAN: before module load',
                        output + 'not ok 1 - normalization'):
            self.assertFalse(runner.assess(changed, 0, 'fragment', 'kunit'))

    def test_gateway_standalone_is_distinct_and_requires_full_evidence(self):
        output = ('VPT1_GATEWAY_TEST: assertions=400 failures=0 PASS\n'
                  'VPT1_GATEWAY_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'gateway'))
        self.assertFalse(runner.assess(output, 0, 'policy'))
        self.assertFalse(runner.assess(output, 0, 'gateway', 'kunit'))
        for changed in (output.replace('400', '349'), output.replace('failures=0', 'failures=1'),
                        output.replace('CPUS=0-1', 'CPUS=0'), output + 'BUG: KASAN: invalid access'):
            self.assertFalse(runner.assess(changed, 0, 'gateway'))

    def test_policy_races_require_complete_clean_result(self):
        output = ('# Subtest: vpt1-policy\nok 1 - vpt1-policy\n'
                  '# vpt1-policy: pass:8 fail:0 skip:0 total:8\n'
                  'VPT1_POLICY_GUEST_PASS\nVPT1_GUEST_CPUS=0-1\n')
        self.assertTrue(runner.assess(output, 0, 'policy', 'kunit'))
        self.assertFalse(runner.assess(output, 0, 'fragment', 'kunit'))
        for changed in (output.replace('pass:8', 'pass:7'),
                        output.replace('skip:0', 'skip:1'), output + 'KCSAN: race',
                        output.replace('VPT1_POLICY_GUEST_PASS', '')):
            self.assertFalse(runner.assess(changed, 0, 'policy', 'kunit'))


if __name__ == '__main__':
    unittest.main()
