# Tagless unicast data path

A multipath ring driver "obviously" tags every frame (HSR does, with a 6-octet tag for
sequence + path). We deliberately do **not** tag the unicast data path.

In v1 there is no resequencer (ADR-0001), the ring is homogeneous, and the ring slaves
are dedicated to Triade — so every field a tag would carry is already available or
unneeded: the **destination MAC** drives forwarding (== my node MAC → deliver and stop;
else relay out the other port), the **source MAC** drives the loop guard via *self-discard*
(`eth.src == my MAC` → drop, bounding circulation to one loop), and **direction** is
implicit in the ingress port.

We accept this because the payoff is large and on the critical path: **no header
push/pop means NIC offloads (GRO/GSO/checksum/RSS) survive on relayed traffic, and the
MTU is completely unaffected** (no baby-jumbo, no reconfiguration of anything behind the
ring) — both of which matter most for the high-bandwidth case the whole project exists to
serve. It also makes the XDP relay program trivial.

Flooded frames (broadcast/multicast/unknown-unicast) are the **exception**: they carry a
16-bit flood-seq inside the Triade control EtherType for dedup, because open-ring
both-direction flooding can otherwise deliver duplicates. Floods are
small, so the header never threatens the MTU.

A per-frame tag returns only when we cross one of two later lines: building the
resequencer (needs a unicast sequence) or phase-B bridging of behind-the-node hosts
(frames whose src/dst are not node MACs, so self-discard and dst-MAC delivery no longer
suffice). The Triade control EtherType is the forward-compatible seam for that.
