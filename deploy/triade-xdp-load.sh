#!/usr/bin/env bash
# Triade XDP relay loader — installed copy.
# Looks for the BPF object at /usr/local/share/triade/triade_xdp.bpf.o.
# Usage: triade-xdp-load.sh [master-iface]  (default: triade0)
set -euo pipefail
MASTER="${1:-triade0}"
OBJ="${TRIADE_BPF_OBJ:-/usr/local/share/triade/triade_xdp.bpf.o}"
PIN="/sys/fs/bpf/triade"

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }
[ -f "$OBJ" ]      || { echo "$OBJ missing" >&2; exit 1; }
command -v bpftool >/dev/null || { echo "bpftool missing" >&2; exit 1; }

ip link show dev "$MASTER" >/dev/null 2>&1 || { echo "no master: $MASTER" >&2; exit 1; }
MASTER_MAC=$(cat /sys/class/net/"$MASTER"/address)
mapfile -t SLAVES < <(ip -o link show master "$MASTER" | awk -F': ' '{print $2}' | sed 's/@.*//')
if [ "${#SLAVES[@]}" -ne 2 ]; then
	echo "expected 2 slaves, found ${#SLAVES[@]}" >&2
	exit 1
fi
IF0="${SLAVES[0]}"; IF1="${SLAVES[1]}"
IDX0=$(cat /sys/class/net/"$IF0"/ifindex)
IDX1=$(cat /sys/class/net/"$IF1"/ifindex)

mountpoint -q /sys/fs/bpf || mount -t bpf bpffs /sys/fs/bpf
[ -d "$PIN" ] && rm -rf "$PIN"
mkdir -p "$PIN"

bpftool prog loadall "$OBJ" "$PIN" type xdp pinmaps "$PIN"

hex_u32_le() { local v=$1; printf '%02x %02x %02x %02x' $((v & 0xff)) $(((v>>8)&0xff)) $(((v>>16)&0xff)) $(((v>>24)&0xff)); }
hex_mac()   { echo "$1" | awk -F: '{for(i=1;i<=6;i++) printf "%s ",tolower($i)}'; }
cfg_value() { printf '%s00 00 %s' "$(hex_mac "$1")" "$(hex_u32_le "$2")"; }

bpftool map update pinned "$PIN/triade_config"   key hex $(hex_u32_le "$IDX0") value hex $(cfg_value "$MASTER_MAC" "$IDX1")
bpftool map update pinned "$PIN/triade_config"   key hex $(hex_u32_le "$IDX1") value hex $(cfg_value "$MASTER_MAC" "$IDX0")
bpftool map update pinned "$PIN/triade_redirect" key hex $(hex_u32_le "$IDX0") value hex $(hex_u32_le "$IDX0")
bpftool map update pinned "$PIN/triade_redirect" key hex $(hex_u32_le "$IDX1") value hex $(hex_u32_le "$IDX1")

attach() {
	local iface=$1
	if bpftool net attach xdpdrv pinned "$PIN/triade_xdp_relay" dev "$iface" 2>/dev/null; then
		echo "  $iface: NATIVE"
	elif bpftool net attach xdpgeneric pinned "$PIN/triade_xdp_relay" dev "$iface" 2>/dev/null; then
		echo "  $iface: GENERIC"
	else
		echo "  $iface: FAILED" >&2
		return 1
	fi
}
attach "$IF0"
attach "$IF1"
echo "triade-xdp attached on $IF0 + $IF1 (master $MASTER)"
