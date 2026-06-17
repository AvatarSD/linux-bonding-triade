# No L2 resequencer in v1 — bet on LAN path skew + TCP RACK

We can use both ring directions for a single fat flow only by splitting its packets
across paths of different length, which reorders them. The obvious fix is a receive-side
L2 resequencer, but that adds head-of-line blocking and is the hardest part of the
driver.

We decided **not** to build the resequencer in v1. The ring is a short-cable LAN, where
per-hop store-and-forward skew is on the order of microseconds; modern Linux TCP
(RACK-TLP) tolerates a few packets of reordering without collapsing its congestion
window. So we ship adaptive **spillover** with **no resequencer** and treat reassembly
as L3's job.

This is a **measurement gate, not a promise**: on real hardware we run a single fat
`iperf3` flow with spillover enabled and watch `ss -ti` for retransmits and cwnd
collapse. If TCP holds its window, the resequencer is never built. If TCP chokes, that
measurement justifies building it *and* sizes the reorder window `W` from the observed
skew. We do not build the hard part on faith.

The known risk: spillover triggers precisely when the shortest path is saturated, i.e.
when the network is busy, so the longer path's pass-through nodes may also be queueing —
inflating skew exactly when we spill. The measurement must be taken under load, not idle.
