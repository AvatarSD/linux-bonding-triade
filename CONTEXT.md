# Triade

A Linux L2 ring driver: 3+ hosts each wired by two NICs to their neighbours form a
ring, and traffic can use both directions of the ring to aggregate bandwidth without
a switch. This glossary fixes the language we use to talk about it.

## Language

**Ring**:
The closed loop of 3 or more nodes, each connected to two neighbours. When the loop
is complete the ring is *closed*; when a link or node fails it becomes *open* (a chain).

**Node**:
One host participating in the ring. Has exactly two ring ports.
_Avoid_: peer, member (use "node").

**Port**:
One of a node's two NICs facing the ring. Distinguished by direction: *clockwise* and
*counter-clockwise*. These are not interchangeable like normal bond slaves — each faces
a different neighbour.
_Avoid_: slave, leg (in user-facing language; "slave port" is fine in kernel code).

**Pass-through**:
A node forwarding a frame that is not addressed to it, out its other port, so the frame
continues around the ring. Distinct from delivery (handing the frame to the local stack).
_Avoid_: relay, bridge (bridging is a separate, later concern).

**Shortest-path forwarding**:
Sending a frame toward a destination in the ring direction with the fewest hops.
The default before any bandwidth-driven splitting.

**Spillover**:
Adaptively sending part of a flow's traffic via the *longer* (other) direction once the
shortest path's link is saturated, to use both directions' bandwidth. Two modes:
*per-flow spillover* (default — move whole flows to the other path; no reordering, helps
only with ≥2 flows) and *per-packet spillover* (opt-in — split a single flow's packets;
the only mode that accelerates one fat flow, and the only one that reorders).
_Avoid_: striping, round-robin (those imply unconditional 50/50 splitting; spillover is
load-triggered and metered).

**Arrival-time equalization**:
The metering rule for per-packet spillover: send just enough overflow down the longer
path that both paths *deliver* in near-order, balancing estimated queueing+propagation
delay rather than packet count. The mechanism that keeps the no-resequencer bet
([ADR-0001](docs/adr/0001-no-l2-resequencer-bet-on-tcp-rack.md)) honest.

**Hop distance**:
The number of pass-through nodes between this node and a destination node, tracked
*per direction* in the node table. The primary path is the lower-hop direction. v1
assumes a homogeneous ring (uniform link speed) so hop distance == path preference; the
metric field is kept pluggable so a bandwidth-weighted cost can replace hop count later
for mixed-speed rings.

**Loop limit (TTL)**:
A pure hop counter on every frame, set to `N-1` and decremented at each pass-through, so
a frame that circles the ring is dropped. Kept separate from hop-distance/path choice —
it is about correctness (killing circulating frames), not path selection.

**Path skew**:
The latency difference between the two ring directions to the same destination (the
longer path traverses more pass-through nodes). The thing that determines how much
reordering spillover causes.

**Supervision frame**:
A periodic announcement a node sends so others can learn ring membership, hop distances,
and detect when the ring opens. Carried in the Triade control EtherType.

**Self-discard**:
The loop guard: a node drops any frame whose Ethernet source address equals its own node
MAC — meaning the frame has travelled all the way around and returned. Bounds any frame's
circulation to a single loop, using a field already present in every frame (so the
unicast data path needs no added tag).

**Triade control EtherType**:
A dedicated EtherType used for the two kinds of frame that aren't plain tagless unicast
data: *supervision frames* and *flooded data frames*. The single control channel; also
the forward-compatible seam where a future resequencer's per-frame sequence would live.

**Flood-seq**:
A 16-bit sequence number carried only on flooded frames (broadcast / multicast /
unknown-unicast, wrapped in the Triade control EtherType). With the source MAC it forms
the dedup key `(src-MAC, flood-seq)`, so a node delivers a flood once even when the open
ring floods both directions. Idle in a closed ring (single-direction flood).
_Avoid_: tag, sequence number (reserve "tag" for the per-frame header we deliberately do
NOT have on the unicast data path).

**Software pass-through** vs **XDP relay**:
Two implementations of pass-through. *Software pass-through* forwards a transit frame in
NAPI context without traversing the stack (the only path available on Thunderbolt, which
lacks native XDP). *XDP relay* redirects transit frames at the driver ingress via a
BPF-map-driven `XDP_REDIRECT` on native-XDP NICs (Intel). The module's control plane
populates the map; the relay itself is dumb and fast.

**Forwarding ceiling**:
The maximum rate at which a node can relay *other* nodes' transit traffic (on top of its
own). The binding constraint on aggregate bandwidth; the scheduler caps spillover at it.

## Example dialogue

> **Dev:** If node B's clockwise port saturates talking to D, we spill onto
> counter-clockwise?
> **Expert:** Right — that's spillover. Shortest-path is clockwise (B→C→D), so we only
> use counter-clockwise (B→A→...→D) for the overflow.
> **Dev:** But counter-clockwise is more hops, so higher path skew — the overflow
> arrives late and out of order.
> **Expert:** Yes. Whether that hurts depends on whether L3 absorbs it or we resequence.
