"""infra/gateway/vpt-gateway.sh `up --wait`: the timeout branch reports the controller's last error.

Runs on any host with bash and python3, without root or Linux: `id`, `ip`, `nft`, `lsmod`,
`sysctl` and `grep` (only for the /proc lookup) are stubs on PATH, and the fake `vptctl` prints
the projection for `network` and a `session ended` line for `run`. The real network setup is
covered by the network-namespace tests.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'infra' / 'gateway' / 'vpt-gateway.sh'
PROJECTION = {'version': 1, 'lan': {'interface': 'wlan0', 'address': '10.77.1.1/24'},
              'underlay': {'interface': 'eth0', 'address': '172.31.250.11/24'},
              'peers': [{'prefixes': ['10.77.2.0/24']}]}


def write(path, text):
    path.write_text(text, newline='\n')
    path.chmod(0o755)


@unittest.skipUnless(shutil.which('bash') and shutil.which('python3'), 'needs bash and python3')
class WaitTimeoutTests(unittest.TestCase):
    def up(self, log_line):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            bindir = tmp / 'bin'
            bindir.mkdir()
            for name in ('ip', 'nft', 'sysctl', 'modprobe', 'insmod'):
                write(bindir / name, '#!/bin/sh\ncat >/dev/null 2>&1 </dev/null || true\nexit 0\n')
            write(bindir / 'id', '#!/bin/sh\necho 0\n')
            write(bindir / 'lsmod', '#!/bin/sh\necho "vpt1_foundation 1 0"\n')
            # controller_pid greps /proc/PID/cmdline, which only exists on Linux.
            real_grep = shutil.which('grep')
            write(bindir / 'grep', '#!/bin/sh\ncase "$*" in */proc/*) exit 0;; esac\nexec %s "$@"\n' % real_grep)
            emit = 'echo %s\n' % json.dumps(log_line) if log_line else ''
            write(tmp / 'vptctl',
                  '#!/bin/sh\nif [ "$1" = network ]; then cat <<"EOF"\n%s\nEOF\nexit 0; fi\n%sexec sleep 4\n'
                  % (json.dumps(PROJECTION), emit))
            config = tmp / 'gw.toml'
            config.write_text('x')
            env = dict(os.environ, PATH='%s:%s' % (bindir, os.environ['PATH']),
                       VPTCTL=str(tmp / 'vptctl'), VPT_RUN_DIR=str(tmp / 'run'))
            try:
                return subprocess.run(['bash', str(SCRIPT), 'up', str(config), '--wait', '1'],
                                      env=env, capture_output=True, text=True, timeout=60)
            finally:
                pidfile = tmp / 'run' / 'controller.pid'
                if pidfile.exists():
                    subprocess.run(['kill', pidfile.read_text().strip()], capture_output=True)

    def test_timeout_reports_last_session_error(self):
        r = self.up('session ended: no response from peer')
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn('tunnel not established after 1s; last error: session ended: no response from peer',
                      r.stderr)

    def test_timeout_without_a_session_error_points_at_the_log(self):
        r = self.up('')
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn('tunnel not established after 1s (peer down?', r.stderr)
        self.assertNotIn('last error', r.stderr)


if __name__ == '__main__':
    unittest.main()
