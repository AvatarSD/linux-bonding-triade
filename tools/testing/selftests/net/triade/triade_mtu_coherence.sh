#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade MTU coherence: when slave MTU goes up to 9000, master MTU may track
# up to (slave_mtu - TRIADE_CTRL_HDR_LEN) for floods. Verify ping with large
# payload survives MTU bumps on both sides.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod ping

tap_plan 3

cleanup() { triade_cleanup_3node; triade_unload_module; }
trap cleanup EXIT

triade_load_module || { tap_not_ok "load module"; tap_summary; exit 1; }
triade_setup_3node
triade_wait_converge 2 5 || { tap_not_ok "ring did not converge"; tap_summary; exit 1; }

# Default MTU is 1500; small ping (size 100) should pass.
if ip netns exec ns-a ping -c2 -W2 -s 100 -q "${TRIADE_ADDRS[1]}" >/dev/null 2>&1; then
	tap_ok "small ping (100 bytes) at default MTU"
else
	tap_not_ok "small ping (100 bytes) at default MTU"
fi

# Bump every slave + master to 9000 / 8992 across all 3 ns.
for i in 0 1 2; do
	ns="${TRIADE_NETNS[$i]}"
	name="${TRIADE_NAMES[$i]}"
	for s in ${TRIADE_SLAVES[$ns]}; do
		ip -netns "$ns" link set "$s" mtu 9000
	done
	ip -netns "$ns" link set "$name" mtu 8992
done
sleep 1

# Large ping (size 8000) should now succeed (don't-fragment to force the
# tested path actually carries the full payload).
if ip netns exec ns-a ping -c2 -W2 -s 8000 -M do -q "${TRIADE_ADDRS[1]}" >/dev/null 2>&1; then
	tap_ok "jumbo ping (8000 bytes, DF) at MTU 8992"
else
	tap_not_ok "jumbo ping (8000 bytes, DF) at MTU 8992"
fi

# Restore MTU.
for i in 0 1 2; do
	ns="${TRIADE_NETNS[$i]}"
	name="${TRIADE_NAMES[$i]}"
	ip -netns "$ns" link set "$name" mtu 1500
	for s in ${TRIADE_SLAVES[$ns]}; do
		ip -netns "$ns" link set "$s" mtu 1500
	done
done

if ip netns exec ns-a ping -c2 -W2 -s 100 -q "${TRIADE_ADDRS[1]}" >/dev/null 2>&1; then
	tap_ok "small ping still works after MTU restore"
else
	tap_not_ok "small ping still works after MTU restore"
fi

tap_summary
