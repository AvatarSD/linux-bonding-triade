/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Triade - L2 multipath ring driver.
 *
 * A ring node has exactly two ports facing its two neighbours. The virtual
 * netdev (triade0) sits on top of them. See ../CONTEXT.md for the language and
 * ../docs/adr/ for the design decisions this code implements.
 *
 * Milestone 1: device + slave plumbing only. No ring forwarding yet (that is
 * Milestone 2, tagless dst-MAC forward + src-MAC self-discard).
 */
#ifndef _TRIADE_H
#define _TRIADE_H

#include <linux/atomic.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define TRIADE_MAX_PORTS	2	/* a ring node always has two neighbours */

/* Scheduler spillover commitment bitmap: one bit per hash bucket. Sized so
 * the table fits in one cache line (256 bits = 32 bytes). Tunable later if
 * we see hash collisions hurting flow isolation.
 */
#define TRIADE_FLOW_BUCKETS	256
#define TRIADE_FLOW_LONGS	(TRIADE_FLOW_BUCKETS / BITS_PER_LONG)

/* Scheduler mode (toggle via debugfs sched_mode file). */
enum triade_sched_mode {
	TRIADE_SCHED_PER_FLOW	= 0,	/* M4 default: per-flow sticky */
	TRIADE_SCHED_PER_PACKET	= 1,	/* M4.5: per-packet ATE; reorders */
	TRIADE_SCHED_FLOW_ECMP	= 2,	/* per-flow hash 50/50 across both
					 * ports, no reordering; lifts the
					 * single-direction aggregate ceiling
					 * with >=2 concurrent flows */
};

/* Forwarding-plane counters. Exposed via debugfs in a later milestone. */
struct triade_stats {
	atomic_long_t	rx_relayed;		/* unicast not-for-me, sent to peer */
	atomic_long_t	rx_self_discard;	/* eth.src == my MAC, dropped */
	atomic_long_t	rx_flood_relayed;	/* flood frames cloned to peer */
	atomic_long_t	tx_flood;		/* local flood frames sent */
	atomic_long_t	rx_super;		/* supervision frames accepted */
	atomic_long_t	rx_flood_dup;		/* flood/super dropped by dedup */
	atomic_long_t	node_aged_out;		/* node table evictions */
	atomic_long_t	tx_spilled;		/* unicast moved off preferred port */
};

struct triade_priv;
struct triade_node_table;	/* opaque - defined in triade_framereg.c */

/* One ring port: a slave netdev facing one neighbour. */
struct triade_port {
	struct net_device	*dev;		/* the slave netdev */
	struct triade_priv	*triade;	/* back-pointer to the master */
	u8			index;		/* 0 or 1 */
	struct rcu_head		rcu;
};

struct dentry;

/* Private state of the triade0 master device. */
struct triade_priv {
	struct net_device		*dev;		/* the master (triade0) */
	struct triade_port __rcu	*port[TRIADE_MAX_PORTS];
	spinlock_t			lock;		/* serialises add/del slave */
	u8				nports;
	struct triade_stats		stats;
	struct triade_node_table	*nodes;		/* M3 node table */
	struct delayed_work		super_work;	/* periodic supervision */
	bool				running;	/* set in ndo_open */
	struct dentry			*debugfs_dir;	/* per-master debugfs */
	unsigned long			flow_alt[TRIADE_FLOW_LONGS];
						/* scheduler bucket commitment */
	u8				sched_mode;	/* enum triade_sched_mode */
	atomic_t			sched_rr;	/* per-packet round-robin counter */
};

#define TRIADE_SUPER_INTERVAL_MS	1000
#define TRIADE_NODE_TIMEOUT_MS		3000

/* triade_slave.c */
int triade_add_slave(struct net_device *dev, struct net_device *slave_dev,
		     struct netlink_ext_ack *extack);
int triade_del_slave(struct net_device *dev, struct net_device *slave_dev);
void triade_release_all_slaves(struct triade_priv *triade);

/* triade_device.c */
extern const struct net_device_ops triade_netdev_ops;
void triade_setup(struct net_device *dev);

/* triade_forward.c - tagless ring forwarding (M2) + control dispatch (M3) */
struct triade_port *triade_peer_port(struct triade_priv *triade,
				     const struct triade_port *self);
rx_handler_result_t triade_forward_rx(struct triade_port *port,
				      struct sk_buff *skb);
/* Re-emit a wrapped frame out @peer with @ttl. Consumes skb. */
void triade_relay_ctrl(struct triade_port *peer, struct sk_buff *skb, u8 ttl);

/* triade_framereg.c - node table + dedup (M3) */
int triade_framereg_init(struct triade_priv *triade);
void triade_framereg_destroy(struct triade_priv *triade);
bool triade_framereg_observe_super(struct triade_priv *triade, u8 port_idx,
				   const u8 *originator, u16 seq, u8 ttl);
bool triade_framereg_observe_flood(struct triade_priv *triade,
				   const u8 *originator, u16 seq);
u16 triade_framereg_next_super_seq(struct triade_priv *triade);
u16 triade_framereg_next_flood_seq(struct triade_priv *triade);
void triade_framereg_age(struct triade_priv *triade, unsigned int timeout_ms);
unsigned int triade_framereg_node_count(struct triade_priv *triade);
int triade_framereg_pref_port(struct triade_priv *triade, const u8 *dst);
void triade_framereg_foreach(struct triade_priv *triade,
			     void (*cb)(const u8 *mac,
					const u16 hop[TRIADE_MAX_PORTS],
					unsigned long last_seen, void *ctx),
			     void *ctx);

/* triade_debugfs.c - /sys/kernel/debug/triade/<ifname>/{stats,nodes} */
int triade_debugfs_module_init(void);
void triade_debugfs_module_exit(void);
int triade_debugfs_add(struct triade_priv *triade);
void triade_debugfs_remove(struct triade_priv *triade);

/* triade_sched.c - per-flow spillover scheduler (M4) */
struct triade_port *triade_sched_pick(struct triade_priv *triade,
				      struct sk_buff *skb,
				      struct triade_port *p0,
				      struct triade_port *p1);

/* triade_super.c - periodic supervision sender + RX (M3) */
void triade_super_start(struct triade_priv *triade);
void triade_super_stop(struct triade_priv *triade);
rx_handler_result_t triade_super_rx(struct triade_port *port,
				    struct sk_buff *skb, u8 ttl, u16 seq,
				    const u8 *originator);

#endif /* _TRIADE_H */
