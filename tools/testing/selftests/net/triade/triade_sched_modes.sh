#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade scheduler modes: writes to debugfs sched_mode accept the valid
# values (0, 1, 2) and reject invalid ones; full-mesh ping still succeeds
# in every mode.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod ping

tap_plan 8  # 3 modes * (set + ping) + 1 reject + 1 reread = roughly

cleanup() { triade_cleanup_3node; triade_unload_module; }
trap cleanup EXIT

triade_load_module || { tap_not_ok "load module"; tap_summary; exit 1; }
triade_setup_3node
triade_wait_converge 2 5 || { tap_not_ok "ring did not converge"; tap_summary; exit 1; }

set_mode_all() {
	local m=$1
	for name in "${TRIADE_NAMES[@]}"; do
		echo "$m" > "$TRIADE_DBG/$name/sched_mode" || return 1
	done
}

read_mode_all_equal() {
	local want=$1
	for name in "${TRIADE_NAMES[@]}"; do
		v=$(cat "$TRIADE_DBG/$name/sched_mode")
		[ "$v" = "$want" ] || return 1
	done
}

for m in 0 1 2; do
	if set_mode_all "$m" && read_mode_all_equal "$m"; then
		tap_ok "sched_mode=$m set+readback on all 3 masters"
	else
		tap_not_ok "sched_mode=$m set+readback on all 3 masters"
	fi
	if triade_ping_all_pairs; then
		tap_ok "all-pairs ping in sched_mode=$m"
	else
		tap_not_ok "all-pairs ping in sched_mode=$m"
	fi
done

# Invalid mode must be rejected with EINVAL.
if echo 99 > "$TRIADE_DBG/${TRIADE_NAMES[0]}/sched_mode" 2>/dev/null; then
	tap_not_ok "sched_mode=99 rejected"
else
	tap_ok "sched_mode=99 rejected with EINVAL"
fi

# Sanity: previous valid mode survives the rejected write.
v=$(cat "$TRIADE_DBG/${TRIADE_NAMES[0]}/sched_mode")
if [ "$v" = "2" ]; then
	tap_ok "rejected write didn't corrupt sched_mode (still 2)"
else
	tap_not_ok "rejected write didn't corrupt sched_mode (got '$v')"
fi

tap_summary
