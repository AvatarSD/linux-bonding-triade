#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade open/close: bring the ab link down -> ns-a <-> ns-b still works via
# ns-c (open ring). Bring it back up -> full mesh restored.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod ping

tap_plan 4

cleanup() { triade_cleanup_3node; triade_unload_module; }
trap cleanup EXIT

triade_load_module || { tap_not_ok "load module"; tap_summary; exit 1; }
triade_setup_3node
triade_wait_converge 2 5 || { tap_not_ok "ring did not converge"; tap_summary; exit 1; }
triade_ping_all_pairs || { tap_not_ok "baseline ping failed"; tap_summary; exit 1; }
tap_ok "baseline closed-ring connectivity"

# Open the a<->b link from ns-a side.
ip -netns ns-a link set ab down
sleep 2

if ip netns exec ns-a ping -c3 -W2 -q "${TRIADE_ADDRS[1]}" >/dev/null 2>&1; then
	tap_ok "ns-a -> ns-b via ns-c (open-ring relay)"
else
	tap_not_ok "ns-a -> ns-b via ns-c (open-ring relay)"
fi

if ip netns exec ns-b ping -c3 -W2 -q "${TRIADE_ADDRS[0]}" >/dev/null 2>&1; then
	tap_ok "ns-b -> ns-a via ns-c (open-ring relay)"
else
	tap_not_ok "ns-b -> ns-a via ns-c (open-ring relay)"
fi

# Reclose.
ip -netns ns-a link set ab up
sleep 2
if triade_ping_all_pairs; then
	tap_ok "full mesh restored after reclose"
else
	tap_not_ok "full mesh restored after reclose"
fi

tap_summary
