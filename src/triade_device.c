// SPDX-License-Identifier: GPL-2.0
/*
 * Triade ring driver - the master netdev (triade0).
 *
 * TX rules (M3):
 *   - multicast/broadcast: wrap in TRIADE_CTRL_FLOOD with a fresh flood-seq,
 *     then send out both carrier-OK ports (one direction would be the M4
 *     optimisation once the node table is rich enough).
 *   - unicast: carrier-aware pick (port 0 preferred, fall back to port 1).
 *     M4 replaces this with the shortest-path scheduler.
 *
 * Supervision starts in ndo_open and stops in ndo_stop. The node table itself
 * lives for the master's lifetime (init/destroy in triade_main.c).
 */
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>

#include "triade.h"
#include "triade_proto.h"

static int triade_open(struct net_device *dev)
{
	struct triade_priv *triade = netdev_priv(dev);

	triade_super_start(triade);
	netif_carrier_on(dev);
	return 0;
}

static int triade_stop(struct net_device *dev)
{
	struct triade_priv *triade = netdev_priv(dev);

	netif_carrier_off(dev);
	triade_super_stop(triade);
	return 0;
}

/* Wrap a user multicast frame in place: insert an 8-byte triade_ctrl_hdr
 * between the L2 header and the L3 payload, rewrite the L2 ethertype to the
 * Triade control type, and stash the original ethertype inside the ctrl hdr.
 */
static int triade_wrap_flood(struct sk_buff *skb, u16 seq)
{
	struct ethhdr *eth;
	struct triade_ctrl_hdr *ctrl;
	__be16 inner_eth;
	int err;

	err = skb_cow(skb, TRIADE_CTRL_HDR_LEN);
	if (err)
		return err;

	eth = eth_hdr(skb);
	inner_eth = eth->h_proto;

	/* Slide the L2 header back into headroom by 8 bytes, freeing 8 bytes
	 * between the (now relocated) L2 and the L3 payload.
	 */
	memmove(skb->data - TRIADE_CTRL_HDR_LEN, skb->data, ETH_HLEN);
	skb_push(skb, TRIADE_CTRL_HDR_LEN);
	skb_reset_mac_header(skb);

	eth = (struct ethhdr *)skb->data;
	eth->h_proto = htons(TRIADE_ETH_P_CTRL);

	ctrl = (struct triade_ctrl_hdr *)(skb->data + ETH_HLEN);
	ctrl->type = TRIADE_CTRL_FLOOD;
	ctrl->version = TRIADE_PROTO_VERSION;
	ctrl->ttl = 1;
	ctrl->_pad = 0;
	ctrl->seq = htons(seq);
	ctrl->inner_ethertype = inner_eth;

	skb->protocol = htons(TRIADE_ETH_P_CTRL);
	return 0;
}

static void triade_send_via(struct net_device *master,
			    struct triade_port *port, struct sk_buff *skb)
{
	dev_sw_netstats_tx_add(master, 1, skb->len);
	skb->dev = port->dev;
	dev_queue_xmit(skb);
}

static bool triade_port_usable(struct triade_port *p)
{
	return p && netif_carrier_ok(p->dev);
}

static netdev_tx_t triade_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct triade_priv *triade = netdev_priv(dev);
	struct triade_port *p0, *p1, *u0, *u1;
	const struct ethhdr *eth = eth_hdr(skb);
	bool flood = is_multicast_ether_addr(eth->h_dest);

	rcu_read_lock();
	p0 = rcu_dereference(triade->port[0]);
	p1 = rcu_dereference(triade->port[1]);
	u0 = triade_port_usable(p0) ? p0 : NULL;
	u1 = triade_port_usable(p1) ? p1 : NULL;

	if (!u0 && !u1) {
		rcu_read_unlock();
		DEV_STATS_INC(dev, tx_dropped);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (flood) {
		u16 seq = triade_framereg_next_flood_seq(triade);

		if (triade_wrap_flood(skb, seq)) {
			rcu_read_unlock();
			DEV_STATS_INC(dev, tx_dropped);
			kfree_skb(skb);
			return NETDEV_TX_OK;
		}
		TRIADE_STAT_INC(triade, tx_flood);

		if (u0 && u1) {
			struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);

			triade_send_via(dev, u0, skb);
			if (clone)
				triade_send_via(dev, u1, clone);
		} else {
			triade_send_via(dev, u0 ? u0 : u1, skb);
		}
	} else {
		struct triade_port *out = triade_sched_pick(triade, skb, u0, u1);

		triade_send_via(dev, out, skb);
	}

	rcu_read_unlock();
	return NETDEV_TX_OK;
}

const struct net_device_ops triade_netdev_ops = {
	.ndo_open		= triade_open,
	.ndo_stop		= triade_stop,
	.ndo_start_xmit		= triade_xmit,
	.ndo_add_slave		= triade_add_slave,
	.ndo_del_slave		= triade_del_slave,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_get_stats64	= dev_get_tstats64,
	.ndo_validate_addr	= eth_validate_addr,
};

void triade_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &triade_netdev_ops;
	dev->needs_free_netdev = true;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;

	/* Master has no real queue of its own; it forwards onto the slaves'
	 * queues.
	 */
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->priv_flags |= IFF_UNICAST_FLT;
	dev->flags |= IFF_MASTER;

	/* Advertise pass-through of common HW offloads. Without these, the
	 * stack runs csum + GSO segmentation in software when packets cross
	 * triade0 - that was the single biggest CPU sink on the sender at
	 * 13 Gb/s (perf: csum_partial_copy_generic ~12 %). The slaves do TSO
	 * and HW checksum natively; we just have to let the kernel push
	 * GSO-formed super-packets down to triade_xmit and forward them to
	 * the slave's dev_queue_xmit unchanged.
	 */
	dev->hw_features = NETIF_F_SG | NETIF_F_HW_CSUM |
			   NETIF_F_GSO_SOFTWARE | NETIF_F_HIGHDMA;
	dev->features |= dev->hw_features;
	dev->vlan_features = dev->hw_features;

	/* Headroom for the M3 flood wrap so we rarely have to skb_cow(). */
	dev->needed_headroom = TRIADE_CTRL_HDR_LEN;

	/* Allow jumbo frames; the operator sets the actual MTU to (slave MTU
	 * - TRIADE_CTRL_HDR_LEN) so wrapped floods still fit on the wire.
	 */
	dev->max_mtu = ETH_MAX_MTU;

	eth_hw_addr_random(dev);
}
