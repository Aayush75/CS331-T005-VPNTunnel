"""Runner must reject partial output, failed cleanup and kernel diagnostics."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'runner', Path(__file__).resolve().parents[2] / 'tools/ay1_qemu_smoke.py')
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)

VALID = '''vpt1_ay1: net_init ns=42 incarnation=1
vpt1_ay1: net_exit ns=42 incarnation=1
AY1_PROVIDER_REJECTION_PASS
AY1_SMOKE_PASS
AY1_GUEST_RESULT=0
'''


class RunnerTests(unittest.TestCase):
    def test_complete(self):
        self.assertTrue(r.assess_output(VALID, 0))

    def test_missing_evidence(self):
        for line in VALID.splitlines():
            self.assertFalse(r.assess_output(VALID.replace(line, ''), 0), line)

    def test_kernel_fault_or_setup_error(self):
        for error in ('BUG:', 'WARNING:', 'Oops:', 'Kernel panic',
                      'refcount_t:', 'AY1_SETUP_ERROR'):
            self.assertFalse(r.assess_output(VALID + error, 0))
        self.assertFalse(r.assess_output(VALID, 1))

    def test_wrong_context_cleanup(self):
        self.assertFalse(r.assess_output(VALID.replace('net_exit ns=42',
                                                      'net_exit ns=43'), 0))


if __name__ == '__main__':
    unittest.main()
