#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade 3-node ring: full-mesh ping, supervision/dedup/self-discard counters.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod ping

tap_plan 8

cleanup() { triade_cleanup_3node; triade_unload_module; }
trap cleanup EXIT

if triade_load_module; then tap_ok "load module"
else tap_not_ok "load module"; tap_summary; exit 1
fi

triade_setup_3node
tap_ok "3-node ring created"

if triade_wait_converge 2 5; then
	tap_ok "node tables converged (each sees 2 peers within 5s)"
else
	tap_not_ok "node tables converged (each sees 2 peers within 5s)"
fi

if triade_ping_all_pairs; then
	tap_ok "all 6 directional pings succeed"
else
	tap_not_ok "all 6 directional pings succeed"
fi

# Each node should have received supervision from both peers.
fail=0
for name in "${TRIADE_NAMES[@]}"; do
	v=$(triade_dbg_get "$name" rx_super); v=${v:-0}
	[ "$v" -gt 1 ] || { tap_diag "$name rx_super=$v (expected >1)"; fail=1; }
done
[ "$fail" = 0 ] && tap_ok "rx_super > 1 on every node" || tap_not_ok "rx_super > 1 on every node"

# Closed ring: every flood arrives twice, second copy must be dedup-dropped.
fail=0
for name in "${TRIADE_NAMES[@]}"; do
	v=$(triade_dbg_get "$name" rx_flood_dup); v=${v:-0}
	[ "$v" -gt 0 ] || { tap_diag "$name rx_flood_dup=$v (expected >0)"; fail=1; }
done
[ "$fail" = 0 ] && tap_ok "rx_flood_dup > 0 on every node" || tap_not_ok "rx_flood_dup > 0 on every node"

# Self-discard fires on at least one node during ARP storm at startup.
saw_self=0
for name in "${TRIADE_NAMES[@]}"; do
	v=$(triade_dbg_get "$name" rx_self_discard); v=${v:-0}
	[ "$v" -gt 0 ] && saw_self=1
done
[ "$saw_self" = 1 ] && tap_ok "rx_self_discard fired (loop guard active)" \
	|| tap_not_ok "rx_self_discard fired (loop guard active)"

# Hops sanity: each node's table must list 2 entries with at least one hop set.
fail=0
for name in "${TRIADE_NAMES[@]}"; do
	hops=$(awk 'NR > 1 && /hops=/ { print }' "$TRIADE_DBG/$name/nodes" 2>/dev/null | wc -l)
	[ "$hops" -ge 2 ] || { tap_diag "$name has $hops nodes (expected >=2)"; fail=1; }
done
[ "$fail" = 0 ] && tap_ok "every node lists >=2 peers in /nodes" \
	|| tap_not_ok "every node lists >=2 peers in /nodes"

tap_summary
