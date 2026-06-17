#!/usr/bin/env bash
# Detach the Triade XDP relay and remove its pinned program/maps.
#
# Usage:   sudo tools/triade-xdp-unload.sh [master-iface]   (default: triade0)

set -euo pipefail

MASTER="${1:-triade0}"
PIN="/sys/fs/bpf/triade"

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }

if ip link show dev "$MASTER" >/dev/null 2>&1; then
	SLAVES=( $(ip -o link show master "$MASTER" 2>/dev/null \
		| awk -F': ' '{print $2}' | sed 's/@.*//') )
	for s in "${SLAVES[@]}"; do
		ip link set dev "$s" xdp off 2>/dev/null || true
		echo "  $s: XDP off"
	done
fi

if [ -d "$PIN" ]; then
	rm -rf "$PIN"
	echo "  removed pinned $PIN"
fi

echo "PASS: XDP unloaded for $MASTER."
