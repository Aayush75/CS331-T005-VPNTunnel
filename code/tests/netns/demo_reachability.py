#!/usr/bin/env python3
"""The two-Pi demo as a test: laptop A cannot reach laptop B, until the VPN is on.

Two gateways (the Pis) are brought up and down only with infra/gateway/vpt-gateway.sh, from
the demo configuration templates (infra/gateway/demo-pi-a.toml and demo-pi-b.toml, with the
interface names and keys of the N1 fixture substituted). Laptops are hosts on each gateway's
LAN; each has a second address, as a DHCP lease would give, to prove the rules cover whole
networks and not fixed hosts.

  VPN never started  laptop A cannot reach laptop B
  VPN on             both directions work: ping, TCP with the original source address, UDP
  VPN on             a laptop still cannot reach the cable (underlay) or anything not in the policy
  one Pi stops       the tunnel closes at once in both directions
  both Pis down      still isolated, then back on again and reachable

Run as root in a disposable VM with vpt1_foundation loaded:
    VPTCTL=/path/to/vptctl demo_reachability.py
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ay2_datapath as D  # noqa: E402  (fixture and namespace helpers)

REPO = D.REPO
VPTCTL = os.environ.get("VPTCTL") or (sys.argv[1] if len(sys.argv) > 1 else "")
SCRIPT = os.path.join(REPO, "infra/gateway/vpt-gateway.sh")
LAPTOP_CHECK = os.path.join(REPO, "infra/gateway/laptop-check.sh")
ns = lambda n: f"vpt_{D.RUN}_{n}"  # noqa: E731  (N1 namespace names, including ha2 and hb2)
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def run(*argv, **kw):
    return subprocess.run(list(argv), capture_output=True, text=True, **kw)


def host(name, *argv, timeout=30):
    return run("ip", "netns", "exec", ns(name), *argv, timeout=timeout)


def ping(src, dst, count=2, wait=1):
    return host(src, "ping", "-c", str(count), "-W", str(wait), dst).returncode == 0


def wait_for(cond, seconds, step=0.25):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(step)
    return cond()


class Pi:
    """One gateway driven only through the bring-up script."""

    def __init__(self, site, config, tmp):
        self.site, self.config = site, config
        self.env = {"VPTCTL": VPTCTL, "VPT_RUN_DIR": os.path.join(tmp, f"run-{site}"), "PATH": os.environ["PATH"]}

    def script(self, action, *extra):
        return subprocess.run(["ip", "netns", "exec", ns(self.site), "env"] + [f"{k}={v}" for k, v in self.env.items()]
                              + ["bash", SCRIPT, action, self.config, *extra],
                              capture_output=True, text=True, timeout=60)

    def log(self):
        try:
            return open(os.path.join(self.env["VPT_RUN_DIR"], "controller.log")).read()
        except OSError:
            return ""


def make_config(template, site, tmp, own_key, peer_pub):
    text = open(os.path.join(REPO, "infra/gateway", template)).read()
    text = re.sub(r'private_key_file = ".*"', f'private_key_file = "{own_key}"', text)
    text = text.replace('"wlan0"', f'"veth_gw{site}_lan"').replace('"eth0"', f'"veth_gw{site}_out"')
    text = re.sub(r'public_key = "[0-9a-f]{64}"', f'public_key = "{peer_pub}"', text)
    path = os.path.join(tmp, f"{template}")
    open(path, "w").write(text)
    return path


def main():
    if os.geteuid() != 0:
        sys.exit("run as root in a disposable VM")
    if not VPTCTL or not os.path.exists(VPTCTL):
        sys.exit("set VPTCTL to the vptctl binary")
    r = run("bash", os.path.join(HERE, "n1_fixture.sh"), "setup", D.RUN)
    if r.returncode:
        sys.exit(f"fixture setup failed: {r.stderr or r.stdout}")
    tmp = tempfile.mkdtemp(prefix="demo_")
    os.chmod(tmp, 0o700)
    servers = []
    pis = []
    try:
        keys, pubs = {}, {}
        for who in ("a", "b"):
            keys[who] = os.path.join(tmp, f"{who}.key")
            out = run(VPTCTL, "genkey", "--out", keys[who]).stdout
            pubs[who] = re.search(r"public_key\s+([0-9a-f]{64})", out).group(1)
        cfg_a = make_config("demo-pi-a.toml", "a", tmp, keys["a"], pubs["b"])
        cfg_b = make_config("demo-pi-b.toml", "b", tmp, keys["b"], pubs["a"])
        for cfg in (cfg_a, cfg_b):
            check("demo-config-validates", run(VPTCTL, "check-config", cfg, "--with-key").returncode == 0,
                  run(VPTCTL, "check-config", cfg, "--with-key").stderr[-300:])
        pi_a, pi_b = Pi("gwa", cfg_a, tmp), Pi("gwb", cfg_b, tmp)
        pis += [pi_a, pi_b]

        # The laptops: a default route through their Pi, and a second ("DHCP") address each.
        for h, gw in (("ha", "10.77.1.1"), ("ha2", "10.77.1.1"), ("hb", "10.77.2.1"), ("hb2", "10.77.2.1")):
            host(h, "ip", "route", "replace", "default", "via", gw)
        host("ha", "ip", "addr", "add", "10.77.1.57/24", "dev", "veth_ha")
        host("hb", "ip", "addr", "add", "10.77.2.88/24", "dev", "veth_hb")

        for h, addr, port, sid in (("hb", "10.77.2.88", 8080, "laptop-b"), ("ha", "10.77.1.57", 8081, "laptop-a")):
            servers.append(subprocess.Popen(
                ["ip", "netns", "exec", ns(h), sys.executable, os.path.join(REPO, "tools/oracles/http_server.py"),
                 "--host", addr, "--port", str(port), "--server-id", sid],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.6)

        def http(src, url, server_id, client_ip, timeout="3"):
            r = host(src, sys.executable, os.path.join(REPO, "tools/oracles/http_client.py"), url,
                     "--expect-server", server_id, "--expect-client-ip", client_ip, "--timeout", timeout)
            return r.returncode == 0, r.stdout[-200:]

        def udp_echo(src, bind_addr, dst, dst_ns):
            srv = subprocess.Popen(["ip", "netns", "exec", ns(dst_ns), sys.executable, "-c",
                                    "import socket;s=socket.socket(2,2);s.bind(('%s',5353));s.settimeout(4)\n"
                                    "d,a=s.recvfrom(2000);s.sendto(d.upper(),a)" % dst],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(0.5)
            code = ("import socket;s=socket.socket(2,2);s.bind(('%s',0));s.settimeout(3)\n"
                    "s.sendto(b'hello',('%s',5353));print(s.recvfrom(100)[0].decode())" % (bind_addr, dst))
            r = host(src, sys.executable, "-c", code)
            srv.terminate()
            return r.stdout.strip() == "HELLO"

        def laptop_check(src, target, port=None):
            r = host(src, "bash", LAPTOP_CHECK, target, *([str(port)] if port else []))
            return r.returncode, r.stdout

        # 1. Nothing started: the laptops cannot see each other.
        rc, out = laptop_check("ha", "10.77.2.88", 8080)
        check("laptop-check-script-says-cannot-see-when-off", rc == 1 and out.count("CANNOT SEE") == 2, out)
        check("never-started-ping-fails", not ping("ha", "10.77.2.10"))
        check("never-started-tcp-fails", not http("ha", "http://10.77.2.88:8080", "laptop-b", "10.77.1.57", "1.5")[0])

        # 2. Both Pis on.
        out_b = pi_b.script("up")
        check("pi-b-up", out_b.returncode == 0, out_b.stdout + out_b.stderr)
        out_a = pi_a.script("up", "--wait", "20")
        check("pi-a-up-and-established", out_a.returncode == 0, out_a.stdout + out_a.stderr + pi_a.log()[-300:])
        check("status-reports-on", "on" in pi_a.script("status").stdout)
        check("laptop-a-reaches-laptop-b", wait_for(lambda: ping("ha", "10.77.2.10"), 10))
        check("laptop-b-reaches-laptop-a", ping("hb", "10.77.1.10"))
        rc, out = laptop_check("ha", "10.77.2.88", 8080)
        check("laptop-check-script-says-can-see-when-on", rc == 0 and out.count("CAN SEE") == 2 and "CANNOT" not in out, out)
        check("second-laptops-too", ping("ha2", "10.77.2.11") and ping("hb2", "10.77.1.11"))
        check("dhcp-style-extra-addresses-too", ping("ha", "10.77.2.88") and ping("hb", "10.77.1.57"))
        check("extra-address-as-the-source-too",
              host("ha", "ping", "-c", "2", "-W", "1", "-I", "10.77.1.57", "10.77.2.88").returncode == 0)
        ok, detail = http("ha", "http://10.77.2.88:8080", "laptop-b", "10.77.1.10")
        check("a-browses-b-and-b-sees-its-address", ok, detail)
        ok, detail = http("hb", "http://10.77.1.57:8081", "laptop-a", "10.77.2.10")
        check("b-browses-a-and-a-sees-its-address", ok, detail)
        check("udp-both-ways", udp_echo("ha", "10.77.1.57", "10.77.2.88", "hb"))

        # 3. What is still not reachable while the VPN is on.
        check("laptop-cannot-reach-the-cable-address", not ping("ha", "172.31.250.12", 1))
        check("laptop-cannot-reach-the-other-pis-underlay-via-a-route", not ping("hb", "172.31.250.11", 1))
        check("unowned-address-not-reachable", not ping("ha", "10.77.3.10", 1))

        # 4. One Pi switched off: the tunnel closes at once in both directions.
        check("a-down", pi_a.script("down").returncode == 0)
        check("status-reports-off", "off" in pi_a.script("status").stdout)
        check("closes-a-to-b", wait_for(lambda: not ping("ha", "10.77.2.10", 1), 4))
        check("closes-b-to-a", not ping("hb", "10.77.1.10", 1))
        check("laptop-check-script-says-cannot-see-after-off", laptop_check("ha", "10.77.2.88", 8080)[0] == 1)

        # 5. Both off: still isolated, including from the cable.
        check("b-down", pi_b.script("down").returncode == 0)
        check("both-off-no-laptop-to-laptop", not ping("ha", "10.77.2.10", 1) and not ping("hb", "10.77.1.10", 1))
        # Give the other Pi a route back, so a leaked packet would really get an answer.
        host("gwb", "ip", "route", "add", "10.77.1.0/24", "via", "172.31.250.11")
        check("both-off-laptop-cannot-reach-the-cable", not ping("ha", "172.31.250.12", 1))
        check("both-off-gateway-forwards-nothing",
              "policy drop" in host("gwa", "nft", "list", "table", "inet", "vpt_gateway").stdout)
        host("gwb", "ip", "route", "del", "10.77.1.0/24", "via", "172.31.250.11")
        check("both-off-no-vpn-device", host("gwa", "ip", "link", "show", "vpn0").returncode != 0)

        # 6. Switch back on.
        check("b-up-again", pi_b.script("up").returncode == 0)
        again = pi_a.script("up", "--wait", "20")
        check("a-up-again", again.returncode == 0, again.stdout + again.stderr + pi_a.log()[-300:])
        check("reachable-again", wait_for(lambda: ping("ha", "10.77.2.10"), 10))
        ok, detail = http("ha", "http://10.77.2.88:8080", "laptop-b", "10.77.1.10")
        check("http-again", ok, detail)
    finally:
        for p in pis:
            p.script("down")
        for s in servers:
            s.terminate()
        run("bash", os.path.join(HERE, "n1_fixture.sh"), "teardown", D.RUN)
    print(f"DEMO_REACHABILITY {'PASS' if not failed else 'FAIL'} passed={passed} failed={failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
