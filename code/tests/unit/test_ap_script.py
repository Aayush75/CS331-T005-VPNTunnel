"""infra/gateway/vpt-ap.sh `render`: the access-point and DHCP settings come from the configuration.

Only `render` is tested; `up` and `down` need hostapd, dnsmasq, a Wi-Fi interface and root, and have
not been run on a Pi.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'infra' / 'gateway' / 'vpt-ap.sh'


def projection(address, interface='wlan0'):
    return {'version': 1, 'lan': {'interface': interface, 'address': address},
            'underlay': {'interface': 'eth0', 'address': '172.31.250.11/24'}, 'peers': []}


@unittest.skipUnless(shutil.which('bash') and shutil.which('python3'), 'needs bash and python3')
class RenderTests(unittest.TestCase):
    def render(self, address, **env):
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / 'vptctl'
            stub.write_text('#!/bin/sh\ncat <<"EOF"\n%s\nEOF\n' % json.dumps(projection(address)),
                            newline='\n')
            stub.chmod(0o755)
            config = Path(tmp) / 'gw.toml'
            config.write_text('x')
            environment = dict(os.environ, VPTCTL=str(stub), VPT_RUN_DIR=tmp, **env)
            return subprocess.run(['bash', str(SCRIPT), 'render', str(config)], env=environment,
                                  capture_output=True, text=True)

    def test_settings_follow_the_configuration(self):
        r = self.render('10.77.1.1/24', VPT_AP_SSID='demo-a', VPT_AP_COUNTRY='IN', VPT_AP_CHANNEL='11')
        self.assertEqual(r.returncode, 0, r.stderr)
        for expected in ('interface=wlan0', 'ssid=demo-a', 'country_code=IN', 'channel=11',
                         'wpa=2', 'rsn_pairwise=CCMP',
                         '--interface=wlan0', '--dhcp-range=10.77.1.50,10.77.1.200,255.255.255.0,12h',
                         '--dhcp-option=option:router,10.77.1.1', '--port=0'):
            self.assertIn(expected, r.stdout)

    def test_other_network_gets_its_own_range(self):
        r = self.render('10.77.2.1/24')
        self.assertIn('--dhcp-range=10.77.2.50,10.77.2.200,255.255.255.0,12h', r.stdout)
        self.assertIn('ssid=vpt-demo-10.77.2.0', r.stdout)

    def test_passphrase_is_never_printed(self):
        r = self.render('10.77.1.1/24', VPT_AP_PASSPHRASE='do-not-print-me')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn('do-not-print-me', r.stdout + r.stderr)
        self.assertIn('wpa_passphrase=<passphrase>', r.stdout)

    def test_network_too_small_for_the_range_is_rejected(self):
        r = self.render('10.77.1.1/25')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('dhcp-range', r.stdout)


if __name__ == '__main__':
    unittest.main()
