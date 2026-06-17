#!/usr/bin/env bash
# Load the Triade XDP relay onto the two slaves of a triade0 master.
#
# Usage:   sudo tools/triade-xdp-load.sh [master-iface]    (default: triade0)
#
# Steps:
#   1. discover master MAC + the two enslaved ring ports
#   2. pin the BPF program and its maps under /sys/fs/bpf/triade
#   3. populate triade_config[port_ifindex] = { mac, peer_ifindex }
#   4. populate triade_redirect (DEVMAP_HASH): peer_ifindex -> peer_ifindex
#   5. attach the program as XDP on both slaves (native mode preferred,
#      generic XDP automatically as a fallback if the driver refuses)
#
# Requires: bpftool, ip(8), built bpf/triade_xdp.bpf.o.
#
# Unload with:  sudo tools/triade-xdp-unload.sh [master-iface]

set -euo pipefail

MASTER="${1:-triade0}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OBJ="$HERE/bpf/triade_xdp.bpf.o"
PIN="/sys/fs/bpf/triade"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }
[ -f "$OBJ" ]      || { echo "build first: make -C bpf"; exit 1; }
command -v bpftool >/dev/null || { echo "bpftool missing (apt install linux-tools-common)"; exit 1; }

# Discover master MAC and slaves.
ip link show dev "$MASTER" >/dev/null 2>&1 || { echo "no master: $MASTER"; exit 1; }
MASTER_MAC=$(cat /sys/class/net/"$MASTER"/address)
SLAVES=( $(ip -o link show master "$MASTER" | awk -F': ' '{print $2}' | sed 's/@.*//') )
if [ "${#SLAVES[@]}" -ne 2 ]; then
	echo "expected 2 slaves under $MASTER, found ${#SLAVES[@]}: ${SLAVES[*]:-}"
	exit 1
fi
IF0="${SLAVES[0]}"; IF1="${SLAVES[1]}"
IDX0=$(cat /sys/class/net/"$IF0"/ifindex)
IDX1=$(cat /sys/class/net/"$IF1"/ifindex)

echo "== Triade XDP load =="
echo "  master:        $MASTER  ($MASTER_MAC)"
echo "  slaves:        $IF0 (#$IDX0)  <-->  $IF1 (#$IDX1)"

# Mount bpffs if needed (Proxmox/Debian usually mount it already at /sys/fs/bpf).
mountpoint -q /sys/fs/bpf || mount -t bpf bpffs /sys/fs/bpf

# Clean prior pin (re-load semantics).
[ -d "$PIN" ] && rm -rf "$PIN"
mkdir -p "$PIN"

echo "== load + pin program and maps =="
bpftool prog loadall "$OBJ" "$PIN" type xdp

# Build hex blobs for bpftool map update. Keys/values are little-endian on x86.
hex_u32_le() {
	local v=$1
	printf '%02x %02x %02x %02x' \
		$(( v        & 0xff)) \
		$(((v >>  8) & 0xff)) \
		$(((v >> 16) & 0xff)) \
		$(((v >> 24) & 0xff))
}
hex_mac() {
	echo "$1" | awk -F: '{for(i=1;i<=6;i++) printf "%s ", tolower($i)}'
}
# struct triade_xdp_cfg = { u8 mac[6]; u8 _pad[2]; u32 peer_ifindex; }
cfg_value() {
	local mac=$1 peer=$2
	printf '%s00 00 %s' "$(hex_mac "$mac")" "$(hex_u32_le "$peer")"
}

echo "== populate triade_config =="
bpftool map update pinned "$PIN/triade_config" \
	key   hex $(hex_u32_le "$IDX0") \
	value hex $(cfg_value "$MASTER_MAC" "$IDX1")
bpftool map update pinned "$PIN/triade_config" \
	key   hex $(hex_u32_le "$IDX1") \
	value hex $(cfg_value "$MASTER_MAC" "$IDX0")

echo "== populate triade_redirect (DEVMAP_HASH) =="
# In DEVMAP_HASH, value can be either the ifindex (u32) or a bpf_devmap_val
# struct; bpftool accepts plain ifindex.
bpftool map update pinned "$PIN/triade_redirect" \
	key   hex $(hex_u32_le "$IDX0") \
	value hex $(hex_u32_le "$IDX0")
bpftool map update pinned "$PIN/triade_redirect" \
	key   hex $(hex_u32_le "$IDX1") \
	value hex $(hex_u32_le "$IDX1")

echo "== attach XDP to slaves =="
attach() {
	local iface=$1
	# Try native (drv) first, fall back to generic.
	if ip -force link set dev "$iface" xdpdrv pinned "$PIN/triade_xdp_relay" 2>/dev/null; then
		echo "  $iface: attached XDP (native/drv)"
	elif ip -force link set dev "$iface" xdp pinned "$PIN/triade_xdp_relay" 2>/dev/null; then
		echo "  $iface: attached XDP (generic)"
	else
		echo "  $iface: FAILED to attach XDP"
		return 1
	fi
}
attach "$IF0"
attach "$IF1"

echo
echo "PASS: XDP relay loaded on $IF0 + $IF1 (master $MASTER)."
echo "Unload with:  sudo tools/triade-xdp-unload.sh $MASTER"
