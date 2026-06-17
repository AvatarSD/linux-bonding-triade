# ADR-0005: XDP map sync from kernel module is deferred to M5.5

## Status

Accepted (2026-06-14).

## Context

[ADR-0004](0004-xdp-relay-with-software-fallback.md) commits to **XDP_REDIRECT
on native-XDP NICs, software fallback everywhere else**. The XDP program (see
`bpf/triade_xdp.bpf.c`) needs three pieces of state from the kernel module:

1. The node's own MAC (for `XDP_DROP` on self-discard, `XDP_PASS` on local
   delivery).
2. The ingress port → peer-port mapping (for `bpf_redirect_map`).
3. (Future) Per-destination shortest-path direction once we want XDP to make
   the same M4 decision that the kernel `triade_sched_pick` makes today.

(1) and (2) are **invariants of the topology**: the master's MAC doesn't move,
and the peer port for ingress P is just "the other port". They change only on
`ndo_add_slave`, `ndo_del_slave` and `ndo_set_mac_address` — events we
already serialise under RTNL.

(3) would require streaming **node-table updates** (M3 supervision data) into
the XDP map every time a neighbour appears, disappears or changes hop
distance. That is a real piece of work: `bpf_map_update_elem` from kernel
context, ownership of the map fd, lifetime tied to the module's lifetime
versus the BPF program's pinning, plus a sync test suite.

## Decision

For M5 v1, **userspace owns the maps**:

- `tools/triade-xdp-load.sh` is run **once** after `triade0` is created and
  its two slaves enslaved.
- It populates `triade_config` and `triade_redirect` from `sysfs`
  (`/sys/class/net/triade0/address`, the slaves' `ifindex`).
- It attaches the XDP program (native first, generic fallback).

The XDP path is therefore restricted in scope:

- **Accelerates:** pass-through unicast (the only relay where the answer is
  always "the other port").
- **Doesn't touch:** supervision, flood dedup, multicast, local delivery —
  the BPF program returns `XDP_PASS` for those so the kernel module's
  software path keeps full authority.

Per-destination map sync from the kernel (the M5.5 work) is deferred until
M4's per-flow scheduler has been measured on real hardware. The expected
shape is:

- Module exposes a BPF map via `bpf_map_create` in `triade_main_init` or
  pins it at module-load time.
- `triade_framereg.c` calls `bpf_map_update_elem` (from process context, not
  softirq) when a node's `hop_via_port[]` changes.
- XDP program gains a third map (`triade_route[dst-MAC] = { pref_port,
  alt_port }`) and uses it on every pass-through.

## Consequences

- **Now:** XDP relay works as a static "swap port" accelerator on Intel NICs
  the moment the loader is run. No code path in the module needs to know
  about BPF.
- **Convergence:** a node failure that requires the multi-hop XDP route to
  change is **not** reflected in the BPF map until the loader is re-run.
  This is fine because pass-through unicast's only choice in the topology
  is invariant (always the opposite port); the software path handles the
  cases where direction matters (supervision, flood).
- **Thunderbolt:** no XDP at all. The software path is the production path
  there — no behavioural change from M2/M3/M4.
- **Maintenance:** the BPF blob and the kernel module move together
  (same repo, same release). Sync drift across module reload + program
  re-attach is the only thing the operator needs to manage; the loader
  script makes it one command.

## Revisit when

- M4 measurement shows pass-through is the bottleneck on real 10G (i.e.
  the scheduler and the software relay together can't keep a 10G link
  saturated). At that point we have hard numbers to justify the kernel-side
  map-sync complexity.
- Or, we want XDP to participate in per-destination scheduling (a use case
  that only matters once nodes ≥ 5 and `hop_via_port[]` becomes a routing
  table rather than a direction-picker).
