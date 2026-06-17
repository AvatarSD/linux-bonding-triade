// SPDX-License-Identifier: GPL-2.0
/*
 * Triade - egress scheduler (M4 + M4.5).
 *
 * Two modes, switchable via debugfs (sched_mode file):
 *
 *   0  PER_FLOW (default, M4)
 *      Per-flow sticky spillover. Each flow goes to its node-table-pref port.
 *      A per-priv "spill bitmap" (one bit per skb_hash bucket) lets us
 *      commit a bucket to the alternate port when pref saturates; commitment
 *      is one-way for the rest of the master's lifetime, so a flow never
 *      flaps. Single TCP flow stays on one port = no reordering, capped at
 *      one-port throughput.
 *
 *   1  PER_PACKET (M4.5)
 *      Arrival-time-equalisation: for every packet, pick the port whose
 *      current qdisc backlog is shorter. Naturally splits a single fat flow
 *      across both ring directions; produces reordering that TCP RACK is
 *      expected to absorb (gated on the ADR-0001 measurement).
 *
 *      Approximation: we use qdisc qlen (packet count) as the arrival-time
 *      proxy. Strict ATE would track per-port "next free slot" timers; the
 *      simpler proxy is adequate for symmetric 10G rings and was the
 *      minimum needed to lift single-flow throughput past one-port line
 *      rate. Refinement is M4.6.
 */
#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <net/sch_generic.h>

#include "triade.h"

#define TRIADE_SCHED_HIGHWATER	64	/* commit-to-alt when pref >= this */

static unsigned int triade_port_backlog(struct triade_port *p)
{
	unsigned int qlen = 0;
	int i;

	for (i = 0; i < p->dev->num_tx_queues; i++) {
		struct netdev_queue *txq = netdev_get_tx_queue(p->dev, i);
		struct Qdisc *q = rcu_dereference_bh(txq->qdisc);

		if (q)
			qlen += READ_ONCE(q->q.qlen);
	}
	return qlen;
}

/* M4 - per-flow sticky spillover. */
static struct triade_port *triade_sched_per_flow(struct triade_priv *triade,
						 struct sk_buff *skb,
						 struct triade_port *p0,
						 struct triade_port *p1,
						 int pref_idx)
{
	struct triade_port *pref, *alt;
	unsigned int pref_bk, alt_bk;
	u32 hash, bucket;
	bool committed_alt;

	if (pref_idx == 0) { pref = p0; alt = p1; }
	else               { pref = p1; alt = p0; }

	hash = skb_get_hash(skb);
	bucket = hash & (TRIADE_FLOW_BUCKETS - 1);
	committed_alt = test_bit(bucket, triade->flow_alt);

	if (committed_alt)
		return alt;	/* sticky one-way */

	pref_bk = triade_port_backlog(pref);
	alt_bk  = triade_port_backlog(alt);
	if (pref_bk > TRIADE_SCHED_HIGHWATER && alt_bk * 2 < pref_bk) {
		if (!test_and_set_bit(bucket, triade->flow_alt))
			TRIADE_STAT_INC(triade, tx_spilled);
		return alt;
	}
	return pref;
}

/* M4.5 - per-packet spread. Round-robin between the two ports by default
 * (which actually splits a single fat flow at near 2x throughput on
 * symmetric K3 rings), but bias toward the less-loaded port when one is
 * sitting on a clearly larger qdisc backlog. The bias term handles
 * asymmetric paths (e.g. 2-hop relay vs 1-hop direct in N>=4 rings) where
 * naive RR would bottleneck on the slower path.
 */
#define TRIADE_SCHED_LOAD_BIAS	16	/* packets of backlog disparity to bias */

static struct triade_port *triade_sched_per_packet(struct triade_priv *triade,
						   struct triade_port *p0,
						   struct triade_port *p1)
{
	unsigned int b0 = triade_port_backlog(p0);
	unsigned int b1 = triade_port_backlog(p1);
	bool pick_p1;

	if (b0 + TRIADE_SCHED_LOAD_BIAS < b1)
		pick_p1 = false;
	else if (b1 + TRIADE_SCHED_LOAD_BIAS < b0)
		pick_p1 = true;
	else
		pick_p1 = (atomic_inc_return(&triade->sched_rr) & 1u) != 0;

	if (pick_p1)
		TRIADE_STAT_INC(triade, tx_spilled);
	return pick_p1 ? p1 : p0;
}

/* Per-flow hash-ECMP: each flow consistently goes to p0 or p1 by hash bit.
 * Two concurrent flows have a 50 % chance of splitting across both ports,
 * which lifts the single-direction aggregate ceiling without reordering.
 */
static struct triade_port *triade_sched_flow_ecmp(struct triade_priv *triade,
						  struct sk_buff *skb,
						  struct triade_port *p0,
						  struct triade_port *p1)
{
	if (skb_get_hash(skb) & 1u) {
		TRIADE_STAT_INC(triade, tx_spilled);
		return p1;
	}
	return p0;
}

struct triade_port *triade_sched_pick(struct triade_priv *triade,
				      struct sk_buff *skb,
				      struct triade_port *p0,
				      struct triade_port *p1)
{
	const struct ethhdr *eth = eth_hdr(skb);
	u8 mode;
	int pref_idx;

	if (!p1)
		return p0;
	if (!p0)
		return p1;

	mode = READ_ONCE(triade->sched_mode);
	if (mode == TRIADE_SCHED_PER_PACKET)
		return triade_sched_per_packet(triade, p0, p1);
	if (mode == TRIADE_SCHED_FLOW_ECMP)
		return triade_sched_flow_ecmp(triade, skb, p0, p1);

	pref_idx = triade_framereg_pref_port(triade, eth->h_dest);
	if (pref_idx < 0)
		pref_idx = (int)(skb_get_hash(skb) & 1u);
	return triade_sched_per_flow(triade, skb, p0, p1, pref_idx);
}
