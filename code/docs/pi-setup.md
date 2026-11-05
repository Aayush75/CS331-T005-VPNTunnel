# Two–Raspberry Pi hardware demonstration

The same system runs on two Raspberry Pi 5s joined by an Ethernet cable, each hosting a
Wi-Fi network for one laptop. Without the VPN, laptop A cannot reach laptop B; with the
VPN up on both Pis, each laptop reaches the other by IP address.

```text
laptop A ~wifi~ Pi A (wlan0 10.77.1.1/24) === cable === Pi B (wlan0 10.77.2.1/24) ~wifi~ laptop B
                     eth0 172.31.250.11                       eth0 172.31.250.12
```

Each Pi's Wi-Fi interface (the laptop LAN) and its Ethernet interface (the cable) are
routed interfaces in different subnets; the Pi never bridges them. The cable is the
gateway-to-gateway transport, not an Internet uplink, and needs no NAT.

Requirements per Pi: Raspberry Pi OS 13 (64-bit), Linux **6.18.y** with headers, a Rust
toolchain, and `build-essential nftables iproute2 hostapd dnsmasq python3 git iw`. The
Pi 5's built-in radio does both the access point and (before the demo) its own campus
Wi-Fi; its radio has one role at a time, so during the demo manage the Pi over the cable
or from its own laptop. Use the official 27 W supply — under-voltage throttles the Pi.

Do all setup while the Pi still has internet.

## 1. Packages

```bash
sudo apt install build-essential nftables iproute2 hostapd dnsmasq python3 git iw
sudo systemctl disable --now hostapd dnsmasq    # the scripts start them
sudo systemctl unmask hostapd
```

## 2. Build on each Pi

```bash
git clone <this repo> && cd <repo>/code
./scripts/pi-build.sh
```

`vptctl` is built natively on the Pi (a dependency needs a C compiler); it takes about a
minute.

## 3. Identities and configuration

```bash
sudo install -d -m 700 /etc/vpntunnel
# on Pi A
sudo ./control/target/release/vptctl genkey --out /etc/vpntunnel/gw-a.key   # prints the public key
# on Pi B
sudo ./control/target/release/vptctl genkey --out /etc/vpntunnel/gw-b.key
```

Edit `infra/gateway/demo-pi-a.toml` on Pi A and `demo-pi-b.toml` on Pi B: set
`private_key_file` to this Pi's key, and set the peer's `public_key` to the **other**
Pi's printed public key. Validate:

```bash
./control/target/release/vptctl check-config infra/gateway/demo-pi-a.toml --with-key
```

Give the cable port a fixed address (Raspberry Pi OS drops an address with no DHCP):

```bash
sudo ip addr add 172.31.250.11/24 dev eth0 && sudo ip link set eth0 up   # .12 on Pi B
```

Confirm the cable before involving the VPN: `ping 172.31.250.12` from Pi A.

## 4. Host each laptop's Wi-Fi

```bash
# Pi A
VPT_AP_SSID=demo-a VPT_AP_PASSPHRASE='choose-one' VPT_AP_COUNTRY=IN \
    ./infra/gateway/vpt-ap.sh up infra/gateway/demo-pi-a.toml
# Pi B: SSID demo-b, its own passphrase
```

This runs hostapd + dnsmasq on the LAN interface (WPA2/CCMP), hands the laptop a DHCP
address in `.50–.200` with the Pi as gateway and no DNS, and does **no** Internet
sharing. The passphrase is read from the environment and never printed. The access point
is independent of the VPN, so the network can be up with the VPN off — that is the
"laptops cannot see each other" half of the demo. Join each laptop to its own Pi's SSID.

## 5. Run the demo

Start Pi B (the responder) first, then Pi A:

```bash
# Pi B
sudo ./scripts/pi-demo.sh up b
# Pi A
sudo ./scripts/pi-demo.sh up a
sudo ./scripts/pi-demo.sh status a
```

`up` loads the module, creates `vpn0`, adds the route to the other Pi's network,
installs the forwarding firewall, and starts the controller. `down` stops the controller
(closing the tunnel at once), removes `vpn0` and the routes, and leaves a firewall that
forwards nothing.

### Show it on the laptops

On a laptop, with the other laptop's address:

```bash
./infra/gateway/laptop-check.sh <other-laptop-ip> [tcp-port]
```

It prints `CAN SEE` / `CANNOT SEE` for a ping and an optional TCP check. Run it with the
VPN down, up, and down again. Start a service to show real traffic crossing, e.g.
`python3 -m http.server 8080` on laptop B, then browse `http://<laptop-B-ip>:8080` from
laptop A.

## 6. Benchmark the physical path (optional)

See [benchmarking.md](benchmarking.md) — `scripts/bench-pi.sh` run from laptop A.
The raw cable baseline (no tunnel) is `scripts/bench-pi-direct.sh`, run on Pi A.

## Notes and limits

- Keep `path_mtu = 1500` in the configuration; a smaller value drops packets silently.
  `vpn0`'s inner MTU is 1428; large UDP datagrams over ~1400 bytes do not cross by design.
- Do not suspend, snapshot, or clone a running gateway: the kernel measures key and lease
  age on the boot clock (which counts while suspended), and a restored snapshot would
  reuse nonce state. Recovery is a fresh handshake, not a resume.
- If a Pi loses power mid-demo, bring B back, then `sudo ./scripts/pi-demo.sh up a` on A
  to start a fresh handshake at once.
- Troubleshooting: `dmesg | tail` for the module, the controller log under the run dir
  for the handshake, and `ping 172.31.250.12` to test the cable independently of the VPN.
