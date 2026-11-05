"""Negative preflight fixtures ensure unsupported environments fail closed."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'preflight', Path(__file__).resolve().parents[2] / 'tools/ay1_preflight.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class PreflightTests(unittest.TestCase):
    def fixture(self):
        config = p.parse_config('\n'.join('CONFIG_' + n + '=y' for n in p.CONFIG))
        symbols = p.parse_symbols('\n'.join(
            '0x1\t' + n + '\tvmlinux\tEXPORT_SYMBOL_GPL' for n in p.SYMBOLS))
        return config, symbols

    def test_supported_build(self):
        self.assertTrue(all(c['status'] == 'PASS' for c in
                            p.assess('6.18.107+deb13-amd64', *self.fixture())))

    def test_pi5_vendor_build(self):
        self.assertTrue(all(c['status'] == 'PASS' for c in
                            p.assess('6.18.34+rpt-rpi-2712', *self.fixture())))

    def test_wrong_family(self):
        for release in ('7.2.9', '6.12.107', '6.19.0', '6.18', '6.18.evil'):
            self.assertEqual(p.assess(release, *self.fixture())[0]['status'], 'FAIL')

    def test_missing_each_requirement(self):
        for name in p.CONFIG:
            config, symbols = self.fixture()
            del config[name]
            self.assertTrue(any(c['status'] == 'FAIL' for c in
                                p.assess('6.18.1', config, symbols)), name)
        for name in p.SYMBOLS:
            config, symbols = self.fixture()
            del symbols[name]
            self.assertTrue(any(c['status'] == 'FAIL' for c in
                                p.assess('6.18.1', config, symbols)), name)

    def test_disabled_config_and_modular_provider(self):
        config, symbols = self.fixture()
        config['CRYPTO_CHACHA20POLY1305'] = 'm'
        self.assertTrue(all(c['status'] == 'PASS' for c in
                            p.assess('6.18.1', config, symbols)))
        config['NET_NS'] = 'n'
        self.assertTrue(any(c['status'] == 'FAIL' for c in
                            p.assess('6.18.1', config, symbols)))


if __name__ == '__main__':
    unittest.main()
