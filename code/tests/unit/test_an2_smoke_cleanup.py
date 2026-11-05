"""Exercise the AN2 runner's success and cleanup-failure reporting."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests/netns/smoke-test.sh"


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "requires Unix bash")
class SmokeCleanupTests(unittest.TestCase):
    def run_fixture(self, fail_cleanup):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            commands = {
                "id": 'printf "0\\n"',
                "modprobe": 'exit 0',
                "ping": 'exit 0',
                "conntrack": 'printf "src=10.99.1.1 dst=10.99.1.2\\n"',
                "nft": 'case "$*" in *"list chain"*) echo "ct state new counter packets 5 bytes 420";; esac',
                "ip": '''
if [[ "$1 $2" == "netns exec" ]]; then shift 3; exec "$@"; fi
if [[ "$1 $2" == "link show" ]]; then exit 1; fi
if [[ "$1 $2" == "netns del" ]]; then
    printf '%s\\n' "$3" >> "$IP_LOG"
    if [[ "$FAIL_CLEANUP" == 1 && "$3" == vpt_smoke_a_* ]]; then exit 1; fi
fi
exit 0
''',
            }
            for name, body in commands.items():
                path = directory / name
                path.write_text("#!/usr/bin/env bash\n" + body + "\n")
                path.chmod(0o755)
            log = directory / "cleanup.log"
            env = dict(os.environ, PATH=str(directory) + os.pathsep + os.environ["PATH"],
                       IP_LOG=str(log), FAIL_CLEANUP=str(int(fail_cleanup)))
            result = subprocess.run(["bash", str(SCRIPT)], env=env, text=True,
                                    capture_output=True, timeout=20, check=False)
            return result, log.read_text().splitlines()

    def test_success_requires_cleanup(self):
        result, removed = self.run_fixture(False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[PASS] AN2 network namespace smoke test and cleanup succeeded!", result.stdout)
        self.assertEqual(len(removed), 2)

    def test_cleanup_failure_fails_and_still_attempts_other_namespace(self):
        result, removed = self.run_fixture(True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to remove test namespace", result.stderr)
        self.assertNotIn("[PASS]", result.stdout)
        self.assertEqual(len(removed), 2)
        self.assertTrue(removed[1].startswith("vpt_smoke_b_"))


if __name__ == "__main__":
    unittest.main()
