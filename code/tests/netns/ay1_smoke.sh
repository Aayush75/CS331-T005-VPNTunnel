#!/bin/sh
# Run inside a disposable VM only; outer runner must impose a timeout.
set -eu
ip_bin=${AY1_IP:-$(command -v ip)}
case "$ip_bin" in /*) ;; *) echo "ERROR: AY1_IP must name an absolute iproute2 path"; exit 2;; esac
ip() { "$ip_bin" "$@"; }
[ "${AY1_DISPOSABLE_VM:-}" = yes ] || { echo 'ERROR: set AY1_DISPOSABLE_VM=yes inside a disposable VM'; exit 2; }
case "$(uname -r)" in 6.18.*) ;; *) echo 'ERROR: Linux 6.18.y required'; exit 2;; esac
[ "$(id -u)" = 0 ] || { echo 'ERROR: root required in guest'; exit 2; }
module_dir=$(cd "${1:-kernel}" && pwd)
[ ! -d /sys/module/vpt1_foundation ] && [ ! -d /sys/module/vpt1_api_probe ] || exit 2
# insmod does not resolve dependencies; the probe needs udp_tunnel. A runner that
# already loaded it (the QEMU initramfs) is left alone; otherwise load it here.
if [ ! -d /sys/module/udp_tunnel ] && ! modprobe -q udp_tunnel; then
    echo 'ERROR: cannot load required module udp_tunnel'; exit 2
fi
prefix="ay1-$$"
a="$prefix-a"
b="$prefix-b"
cleanup() {
    rc=$?
    trap - EXIT INT TERM
    for ns in "$a" "$b"; do
        if ip netns list | grep -q "^$ns\( \|$\)"; then
            ip netns delete "$ns" || rc=1
        fi
    done
    if [ -d /sys/module/vpt1_api_probe ]; then rmmod vpt1_api_probe || rc=1; fi
    if [ -d /sys/module/vpt1_foundation ]; then rmmod vpt1_foundation || rc=1; fi
    [ "$rc" = 0 ] && echo 'AY1_SMOKE_PASS'
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Exercise namespaces that predate module registration as well as later ones.
ip netns add "$a"
insmod "$module_dir/vpt1_foundation.ko"
insmod "$module_dir/vpt1_api_probe.ko"
ip netns add "$b"
for ns in "$a" "$b"; do
    ip -n "$ns" link add vpn0 type vpt1_ay1
    ip -n "$ns" -d link show vpn0
    if ip -n "$ns" link add extra type vpt1_ay1; then
        echo 'FAIL: duplicate gateway device accepted'; exit 1
    fi
    # Both namespaces have their own loopback UDP 51888 socket.
    ip netns exec "$ns" grep -q '0100007F:CAB0 ' /proc/net/udp
    if ip -n "$ns" link set vpn0 netns 1; then
        echo 'FAIL: namespace migration accepted'; exit 1
    fi
done
ip -n "$a" link delete vpn0
ip -n "$a" link add vpn0 type vpt1_ay1
# Deleting a namespace with a live device/socket must eventually run per-net exit.
ip netns delete "$b"
ip netns add "$b"
ip -n "$b" link add vpn0 type vpt1_ay1
# Unregister link kind with live devices; namespace contexts remain until unload.
rmmod vpt1_api_probe
rmmod vpt1_foundation
if ip -n "$a" link show vpn0; then echo 'FAIL: device survived unload'; exit 1; fi
if ip -n "$b" link show vpn0; then echo 'FAIL: device survived unload'; exit 1; fi
# Second load/unload cycle catches stale registrations and socket binds.
insmod "$module_dir/vpt1_foundation.ko"
insmod "$module_dir/vpt1_api_probe.ko"
rmmod vpt1_api_probe
rmmod vpt1_foundation
