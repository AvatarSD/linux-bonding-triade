#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Triade smoke: module load, single-master create+destroy, debugfs visible.
. "$(dirname "$0")/lib.sh"

triade_require_root
triade_require_ko
triade_require_cmds ip rmmod insmod

tap_plan 5

trap 'triade_unload_module' EXIT

if triade_load_module; then
	tap_ok "module loads"
else
	tap_not_ok "module loads"
	tap_summary; exit 1
fi

if [ -d "$TRIADE_DBG" ]; then
	tap_ok "debugfs root /sys/kernel/debug/triade exists"
else
	tap_not_ok "debugfs root /sys/kernel/debug/triade exists"
fi

if ip link add smoke0 type triade 2>/dev/null; then
	tap_ok "ip link add type triade succeeds"
else
	tap_not_ok "ip link add type triade succeeds"
fi

if [ -d "$TRIADE_DBG/smoke0" ] && \
   [ -e "$TRIADE_DBG/smoke0/stats" ] && \
   [ -e "$TRIADE_DBG/smoke0/nodes" ] && \
   [ -e "$TRIADE_DBG/smoke0/sched_mode" ]; then
	tap_ok "per-master debugfs files exist"
else
	tap_not_ok "per-master debugfs files exist"
fi

if ip link del smoke0 2>/dev/null && [ ! -d "$TRIADE_DBG/smoke0" ]; then
	tap_ok "ip link del cleans up debugfs"
else
	tap_not_ok "ip link del cleans up debugfs"
fi

tap_summary
