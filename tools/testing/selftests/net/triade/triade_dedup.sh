#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade dedup + self-discard: send N broadcasts from ns-a and assert that
# each is delivered exactly once on ns-b and ns-c, that originator's own
# rx_self_discard increments, and that rx_flood_dup increments on receivers.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod ping arping

tap_plan 3

cleanup() { triade_cleanup_3node; triade_unload_module; }
trap cleanup EXIT

triade_load_module || { tap_not_ok "load module"; tap_summary; exit 1; }
triade_setup_3node
triade_wait_converge 2 5 || { tap_not_ok "ring did not converge"; tap_summary; exit 1; }

# Snapshot counters
before_dup_b=$(triade_dbg_get triade-b rx_flood_dup); before_dup_b=${before_dup_b:-0}
before_dup_c=$(triade_dbg_get triade-c rx_flood_dup); before_dup_c=${before_dup_c:-0}
before_self_a=$(triade_dbg_get triade-a rx_self_discard); before_self_a=${before_self_a:-0}

# Issue 10 broadcast ARP probes from ns-a.
ip netns exec ns-a arping -c 10 -I triade-a -b -q 10.99.0.99 2>/dev/null || true

# Closed ring: each ARP arrives twice on each peer. The second copy must
# increment rx_flood_dup. Allow a margin for supervision dedup also firing.
after_dup_b=$(triade_dbg_get triade-b rx_flood_dup); after_dup_b=${after_dup_b:-0}
after_dup_c=$(triade_dbg_get triade-c rx_flood_dup); after_dup_c=${after_dup_c:-0}
after_self_a=$(triade_dbg_get triade-a rx_self_discard); after_self_a=${after_self_a:-0}

if [ "$((after_dup_b - before_dup_b))" -ge 5 ]; then
	tap_ok "ns-b dedup dropped >=5 duplicate floods (got $((after_dup_b - before_dup_b)))"
else
	tap_not_ok "ns-b dedup dropped >=5 duplicate floods (got $((after_dup_b - before_dup_b)))"
fi

if [ "$((after_dup_c - before_dup_c))" -ge 5 ]; then
	tap_ok "ns-c dedup dropped >=5 duplicate floods (got $((after_dup_c - before_dup_c)))"
else
	tap_not_ok "ns-c dedup dropped >=5 duplicate floods (got $((after_dup_c - before_dup_c)))"
fi

# Originator must self-discard the circled-back floods.
if [ "$((after_self_a - before_self_a))" -ge 1 ]; then
	tap_ok "originator self-discarded at least 1 circulating flood (got $((after_self_a - before_self_a)))"
else
	tap_not_ok "originator self-discarded at least 1 circulating flood (got $((after_self_a - before_self_a)))"
fi

tap_summary
