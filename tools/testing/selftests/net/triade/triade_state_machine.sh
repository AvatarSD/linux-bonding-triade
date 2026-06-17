#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade state machine: NETDEV_DOWN of a slave should invalidate the hop
# column for that port within milliseconds (no waiting for the 3 s aging
# timeout). NETDEV_UP should kick a supervision frame so the hop column
# repopulates well under the 1 s normal cadence.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod

tap_plan 4

cleanup() { triade_cleanup_3node; triade_unload_module; }
trap cleanup EXIT

triade_load_module || { tap_not_ok "load module"; tap_summary; exit 1; }
triade_setup_3node
triade_wait_converge 2 5 || { tap_not_ok "ring did not converge"; tap_summary; exit 1; }

# Helper: hop for $peer_mac via $port on $master ns. Empty if not learned.
hop_via() {
	local ns=$1 master=$2 peer_mac=$3 port=$4
	awk -v mac="$peer_mac" -v p="$port" '
		!/^#/ && $1 == mac {
			match($0, /hops=\[[^]]*\]/);
			s = substr($0, RSTART+6, RLENGTH-7);
			n = split(s, arr, ",");
			print arr[p+1];
		}
	' "$TRIADE_DBG/$master/nodes" 2>/dev/null
}

# ns-a has slaves ab (port 0 facing ns-b) and ac (port 1 facing ns-c).
NS=ns-a
MASTER=triade-a
PEER_B_MAC=$(ip netns exec ns-b cat /sys/class/net/triade-b/address)
PEER_C_MAC=$(ip netns exec ns-c cat /sys/class/net/triade-c/address)

# Before: ns-b should be visible directly via port 0 (slave ab).
h=$(hop_via "$NS" "$MASTER" "$PEER_B_MAC" 0)
if [ "$h" = "1" ]; then
	tap_ok "baseline: ns-b is hops[0]=1 on triade-a"
else
	tap_not_ok "baseline: ns-b is hops[0]=1 on triade-a (got '$h')"
fi

# Bring port 0 (slave ab) DOWN. Hop column 0 must invalidate to '-' immediately.
ip -netns ns-a link set ab down
# Tight bound: invalidate runs inside the NETDEV_DOWN notifier path so the
# column should be cleared by the time link-set returns.
h=$(hop_via "$NS" "$MASTER" "$PEER_B_MAC" 0)
if [ "$h" = "-" ] || [ -z "$h" ]; then
	tap_ok "after DOWN: hops[0] invalidated to '-' (no aging wait)"
else
	tap_not_ok "after DOWN: hops[0] invalidated to '-' (got '$h')"
fi

# After 3-4 s, supervision should re-discover ns-b via the long path
# (port 1 -> ns-c -> port 0 of ns-c -> ns-b), so hops[1] for ns-b should be 2.
sleep 4
h=$(hop_via "$NS" "$MASTER" "$PEER_B_MAC" 1)
if [ "$h" = "2" ]; then
	tap_ok "open-ring: ns-b now reachable as hops[1]=2 (via ns-c relay)"
else
	tap_not_ok "open-ring: ns-b reachable as hops[1]=2 (got '$h')"
fi

# Bring port 0 back UP. triade_super_kick should fire a supervision frame
# essentially immediately, so hops[0] should be back to 1 well under 1 s.
ip -netns ns-a link set ab up
sleep 1
h=$(hop_via "$NS" "$MASTER" "$PEER_B_MAC" 0)
if [ "$h" = "1" ]; then
	tap_ok "after UP+1s: hops[0]=1 restored (supervision kick fired)"
else
	tap_not_ok "after UP+1s: hops[0]=1 restored (got '$h')"
fi

tap_summary
