// SPDX-License-Identifier: GPL-2.0
/*
 * Triade - the node table + per-originator dedup (M3).
 *
 * Each ring neighbour we hear from (via supervision or wrapped flood) gets one
 * struct triade_node, hashed by MAC. The entry remembers:
 *   - which port saw the originator and at what hop distance (filled by
 *     supervision; consumed by the M4 scheduler);
 *   - last-seen jiffies (the dual-signal timeout's "not local" half);
 *   - two sliding-window dedup states, one for supervision-seq, one for
 *     flood-seq. The window is 32 wide; older seqs are assumed seen.
 *
 * Lookups are RCU-protected. Inserts/updates take a per-table spinlock; this
 * is a low-rate path (supervision is 1/s and floods are dominated by ARP/ND).
 */
#include <linux/etherdevice.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "triade.h"
#include "triade_proto.h"

#define TRIADE_NODE_HASH_BITS	4
#define TRIADE_NODE_HASH_SIZE	(1u << TRIADE_NODE_HASH_BITS)
#define TRIADE_DEDUP_WINDOW	32

/* Sliding-window dedup state for one (originator, channel). */
struct triade_dedup {
	u16	last_seq;	/* highest seq observed */
	u32	bitmap;		/* bit i set => seq (last_seq - i) seen */
};

/* One observed originator. */
struct triade_node {
	u8			mac[ETH_ALEN];
	u16			hop_via_port[TRIADE_MAX_PORTS]; /* 0xFFFF = unknown */
	unsigned long		last_seen;
	struct triade_dedup	super_dedup;
	struct triade_dedup	flood_dedup;
	struct hlist_node	hnode;
	struct rcu_head		rcu;
};

struct triade_node_table {
	struct hlist_head	bucket[TRIADE_NODE_HASH_SIZE];
	spinlock_t		lock;	/* writers; readers use RCU */
	atomic_t		super_tx_seq;
	atomic_t		flood_tx_seq;
};

static u32 mac_bucket(const u8 *mac)
{
	return jhash(mac, ETH_ALEN, 0) & (TRIADE_NODE_HASH_SIZE - 1);
}

/* Sliding-window dedup. last_seq = highest seq seen; bitmap bit i set means
 * seq (last_seq - i) has been observed. Returns true if @seq is NEW.
 *
 * Caller serialises against concurrent writers (per-node, via priv->lock here).
 */
static bool triade_dedup_check(struct triade_dedup *d, u16 seq, bool first)
{
	s32 diff;

	if (first) {
		d->last_seq = seq;
		d->bitmap = 1u;
		return true;
	}

	diff = (s32)(s16)(seq - d->last_seq);
	if (diff > 0) {
		if (diff >= TRIADE_DEDUP_WINDOW)
			d->bitmap = 1u;
		else
			d->bitmap = (d->bitmap << diff) | 1u;
		d->last_seq = seq;
		return true;
	}
	if (diff == 0) {
		if (d->bitmap & 1u)
			return false;
		d->bitmap |= 1u;
		return true;
	}
	/* diff < 0: an older seq inside the window */
	if (-diff < TRIADE_DEDUP_WINDOW) {
		u32 mask = 1u << -diff;

		if (d->bitmap & mask)
			return false;
		d->bitmap |= mask;
		return true;
	}
	return false;	/* too old, treat as duplicate */
}

static struct triade_node *node_lookup_locked(struct triade_node_table *t,
					      const u8 *mac)
{
	struct triade_node *n;

	hlist_for_each_entry(n, &t->bucket[mac_bucket(mac)], hnode) {
		if (ether_addr_equal(n->mac, mac))
			return n;
	}
	return NULL;
}

static struct triade_node *node_create(const u8 *mac)
{
	struct triade_node *n;
	int i;

	n = kzalloc(sizeof(*n), GFP_ATOMIC);
	if (!n)
		return NULL;
	ether_addr_copy(n->mac, mac);
	for (i = 0; i < TRIADE_MAX_PORTS; i++)
		n->hop_via_port[i] = 0xFFFF;
	n->last_seen = jiffies;
	return n;
}

int triade_framereg_init(struct triade_priv *triade)
{
	struct triade_node_table *t;
	int i;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	spin_lock_init(&t->lock);
	for (i = 0; i < TRIADE_NODE_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&t->bucket[i]);
	atomic_set(&t->super_tx_seq, 0);
	atomic_set(&t->flood_tx_seq, 0);
	triade->nodes = t;
	return 0;
}

void triade_framereg_destroy(struct triade_priv *triade)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	struct hlist_node *tmp;
	int i;

	if (!t)
		return;
	spin_lock_bh(&t->lock);
	for (i = 0; i < TRIADE_NODE_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(n, tmp, &t->bucket[i], hnode) {
			hlist_del_rcu(&n->hnode);
			kfree_rcu(n, rcu);
		}
	}
	spin_unlock_bh(&t->lock);
	/* Free the table after one grace period so RCU readers finish. */
	synchronize_rcu();
	kfree(t);
	triade->nodes = NULL;
}

/* Observe a supervision frame from @originator received on @port_idx with
 * @ttl hops. Returns true if the frame is NEW (caller should relay it).
 */
bool triade_framereg_observe_super(struct triade_priv *triade, u8 port_idx,
				   const u8 *originator, u16 seq, u8 ttl)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	bool first, fresh;

	spin_lock_bh(&t->lock);
	n = node_lookup_locked(t, originator);
	first = !n;
	if (!n) {
		n = node_create(originator);
		if (!n) {
			spin_unlock_bh(&t->lock);
			return false;
		}
		hlist_add_head_rcu(&n->hnode, &t->bucket[mac_bucket(originator)]);
	}
	fresh = triade_dedup_check(&n->super_dedup, seq, first);
	if (fresh) {
		n->last_seen = jiffies;
		if (port_idx < TRIADE_MAX_PORTS)
			n->hop_via_port[port_idx] = ttl;
	}
	spin_unlock_bh(&t->lock);
	return fresh;
}

/* Observe a wrapped flood from @originator with @seq. Returns true if NEW. */
bool triade_framereg_observe_flood(struct triade_priv *triade,
				   const u8 *originator, u16 seq)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	bool first, fresh;

	spin_lock_bh(&t->lock);
	n = node_lookup_locked(t, originator);
	first = !n;
	if (!n) {
		n = node_create(originator);
		if (!n) {
			spin_unlock_bh(&t->lock);
			return false;
		}
		hlist_add_head_rcu(&n->hnode, &t->bucket[mac_bucket(originator)]);
	}
	fresh = triade_dedup_check(&n->flood_dedup, seq, first);
	if (fresh)
		n->last_seen = jiffies;
	spin_unlock_bh(&t->lock);
	return fresh;
}

u16 triade_framereg_next_super_seq(struct triade_priv *triade)
{
	return (u16)atomic_inc_return(&triade->nodes->super_tx_seq);
}

u16 triade_framereg_next_flood_seq(struct triade_priv *triade)
{
	return (u16)atomic_inc_return(&triade->nodes->flood_tx_seq);
}

/* Age out nodes whose last_seen is older than @timeout_ms. */
void triade_framereg_age(struct triade_priv *triade, unsigned int timeout_ms)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	struct hlist_node *tmp;
	unsigned long limit = jiffies - msecs_to_jiffies(timeout_ms);
	int i;

	spin_lock_bh(&t->lock);
	for (i = 0; i < TRIADE_NODE_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(n, tmp, &t->bucket[i], hnode) {
			if (time_before(n->last_seen, limit)) {
				hlist_del_rcu(&n->hnode);
				kfree_rcu(n, rcu);
				TRIADE_STAT_INC(triade, node_aged_out);
			}
		}
	}
	spin_unlock_bh(&t->lock);
}

/* Clear the @port_idx column across every node so the scheduler's pref-port
 * query stops returning a port that just lost carrier. Supervision will
 * repopulate the column once carrier is back and the neighbour's frames
 * arrive again.
 */
void triade_framereg_invalidate_port(struct triade_priv *triade, u8 port_idx)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	int i;

	if (port_idx >= TRIADE_MAX_PORTS || !t)
		return;

	spin_lock_bh(&t->lock);
	for (i = 0; i < TRIADE_NODE_HASH_SIZE; i++) {
		hlist_for_each_entry(n, &t->bucket[i], hnode)
			WRITE_ONCE(n->hop_via_port[port_idx], 0xFFFF);
	}
	spin_unlock_bh(&t->lock);
}

/* Return the port index (0/1) with the shorter learned hop distance to @dst,
 * or -1 if @dst isn't in the node table. Ties go to port 0.
 */
int triade_framereg_pref_port(struct triade_priv *triade, const u8 *dst)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	int pref = -1;
	u16 h0, h1;

	rcu_read_lock();
	n = NULL;
	hlist_for_each_entry_rcu(n, &t->bucket[mac_bucket(dst)], hnode) {
		if (ether_addr_equal(n->mac, dst))
			break;
	}
	if (n && ether_addr_equal(n->mac, dst)) {
		h0 = READ_ONCE(n->hop_via_port[0]);
		h1 = READ_ONCE(n->hop_via_port[1]);
		if (h0 == 0xFFFF && h1 == 0xFFFF)
			pref = -1;
		else if (h0 == 0xFFFF)
			pref = 1;
		else if (h1 == 0xFFFF)
			pref = 0;
		else
			pref = (h1 < h0) ? 1 : 0;
	}
	rcu_read_unlock();
	return pref;
}

/* Walk the node table under RCU, invoking @cb for each entry. The callback
 * must not sleep and must not call back into the framereg (the table lock is
 * NOT held; entries may disappear under RCU after the callback returns).
 */
void triade_framereg_foreach(struct triade_priv *triade,
			     void (*cb)(const u8 *mac,
					const u16 hop[TRIADE_MAX_PORTS],
					unsigned long last_seen, void *ctx),
			     void *ctx)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	int i;

	rcu_read_lock();
	for (i = 0; i < TRIADE_NODE_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(n, &t->bucket[i], hnode)
			cb(n->mac, n->hop_via_port, n->last_seen, ctx);
	}
	rcu_read_unlock();
}

unsigned int triade_framereg_node_count(struct triade_priv *triade)
{
	struct triade_node_table *t = triade->nodes;
	struct triade_node *n;
	unsigned int count = 0;
	int i;

	rcu_read_lock();
	for (i = 0; i < TRIADE_NODE_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(n, &t->bucket[i], hnode)
			count++;
	}
	rcu_read_unlock();
	return count;
}
