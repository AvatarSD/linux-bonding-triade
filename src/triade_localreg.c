// SPDX-License-Identifier: GPL-2.0
/*
 * Triade - local-MAC registry (bridged-master support).
 *
 * The M2 forwarding rules identify a node's own L2 identity by a single
 * address: the triade0 master MAC. self-discard fires on src == master;
 * local delivery fires on dst == master; everything else is relayed onward.
 * That is correct only when the node's IP sits *directly* on triade0 (one MAC
 * per node - ADR-0002).
 *
 * When triade0 is enslaved into a bridge (e.g. a Proxmox vmbr0 trunking VLANs
 * across the ring), the node's L2 identity is no longer one MAC: every VM tap,
 * per-VLAN SVI and other bridge port lives *behind* triade0 with its own MAC.
 * Under the single-MAC rules those foreign-but-local MACs are mis-forwarded:
 *
 *   - a unicast addressed to a behind-bridge local MAC (dst != master) is not
 *     recognised as local, so it is relayed back onto the ring -> black-hole;
 *   - a frame the node originated (src = a behind-bridge local MAC, not the
 *     master) that circles the ring is not self-discarded -> it loops until a
 *     bridge drops it as its own source address.
 *
 * This registry learns the set of local MACs from the master's TX egress:
 * every source MAC that leaves triade0 is, by definition, local. The bridge
 * never forwards a frame back out the port it arrived on (so ring-ingress
 * frames are never re-emitted onto triade0), and the module's own relays go
 * straight to the slave via dev_queue_xmit, bypassing triade0's
 * ndo_start_xmit - so only genuinely local sources reach the learn hook.
 *
 * The forwarding plane then treats "src/dst is a known local MAC" exactly like
 * "== master MAC". Entries age out on the supervision cadence (a MAC that goes
 * quiet for longer than the aging window is re-learned on its next frame,
 * matching a learning bridge's FDB). See docs/adr/0006.
 */
#include <linux/etherdevice.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "triade.h"

#define TRIADE_LOCAL_HASH_BITS	8
#define TRIADE_LOCAL_HASH_SIZE	(1u << TRIADE_LOCAL_HASH_BITS)
/* Bound the table so MAC churn / spoofing can't grow it without limit. Well
 * above any realistic behind-bridge local-MAC count (VM taps + SVIs).
 */
#define TRIADE_LOCAL_MAX	1024

struct triade_local {
	u8			mac[ETH_ALEN];
	unsigned long		last_seen;
	struct hlist_node	hnode;
	struct rcu_head		rcu;
};

struct triade_local_table {
	struct hlist_head	bucket[TRIADE_LOCAL_HASH_SIZE];
	spinlock_t		lock;	/* writers; readers use RCU */
	atomic_t		count;
};

static u32 local_bucket(const u8 *mac)
{
	return jhash(mac, ETH_ALEN, 0) & (TRIADE_LOCAL_HASH_SIZE - 1);
}

int triade_localreg_init(struct triade_priv *triade)
{
	struct triade_local_table *t;
	int i;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	spin_lock_init(&t->lock);
	for (i = 0; i < TRIADE_LOCAL_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&t->bucket[i]);
	atomic_set(&t->count, 0);
	triade->locals = t;
	return 0;
}

void triade_localreg_destroy(struct triade_priv *triade)
{
	struct triade_local_table *t = triade->locals;
	struct triade_local *e;
	struct hlist_node *tmp;
	int i;

	if (!t)
		return;
	spin_lock_bh(&t->lock);
	for (i = 0; i < TRIADE_LOCAL_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(e, tmp, &t->bucket[i], hnode) {
			hlist_del_rcu(&e->hnode);
			kfree_rcu(e, rcu);
		}
	}
	spin_unlock_bh(&t->lock);
	/* Free the table after a grace period so RCU readers finish. */
	synchronize_rcu();
	kfree(t);
	triade->locals = NULL;
}

/* RCU-safe membership test. Called from the forwarding hot path (softirq,
 * already under rcu_read_lock via the rx_handler); the nested rcu_read_lock is
 * cheap and keeps the function safe from any caller.
 */
bool triade_localreg_is_local(struct triade_priv *triade, const u8 *mac)
{
	struct triade_local_table *t = triade->locals;
	struct triade_local *e;
	bool found = false;

	if (!t)
		return false;

	rcu_read_lock();
	hlist_for_each_entry_rcu(e, &t->bucket[local_bucket(mac)], hnode) {
		if (ether_addr_equal(e->mac, mac)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

/* Learn @mac as local (a source MAC seen egressing triade0). Hot path: the
 * common case is "already known", handled lockless with an RCU lookup + a
 * last_seen refresh. Only the first sighting of a MAC takes the table lock.
 */
void triade_localreg_learn(struct triade_priv *triade, const u8 *mac)
{
	struct triade_local_table *t = triade->locals;
	struct triade_local *e, *ex;
	u32 b;

	if (!t || !is_valid_ether_addr(mac))
		return;

	b = local_bucket(mac);

	rcu_read_lock();
	hlist_for_each_entry_rcu(e, &t->bucket[b], hnode) {
		if (ether_addr_equal(e->mac, mac)) {
			WRITE_ONCE(e->last_seen, jiffies);
			rcu_read_unlock();
			return;
		}
	}
	rcu_read_unlock();

	if (atomic_read(&t->count) >= TRIADE_LOCAL_MAX)
		return;			/* bounded; aging reclaims room */

	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return;
	ether_addr_copy(e->mac, mac);
	e->last_seen = jiffies;

	spin_lock_bh(&t->lock);
	/* Re-check under the lock: a racing learner may have inserted it. */
	hlist_for_each_entry(ex, &t->bucket[b], hnode) {
		if (ether_addr_equal(ex->mac, mac)) {
			spin_unlock_bh(&t->lock);
			kfree(e);
			return;
		}
	}
	hlist_add_head_rcu(&e->hnode, &t->bucket[b]);
	atomic_inc(&t->count);
	spin_unlock_bh(&t->lock);
}

/* Age out local MACs unheard-from for longer than @timeout_ms. */
void triade_localreg_age(struct triade_priv *triade, unsigned int timeout_ms)
{
	struct triade_local_table *t = triade->locals;
	struct triade_local *e;
	struct hlist_node *tmp;
	unsigned long limit = jiffies - msecs_to_jiffies(timeout_ms);
	int i;

	if (!t)
		return;
	spin_lock_bh(&t->lock);
	for (i = 0; i < TRIADE_LOCAL_HASH_SIZE; i++) {
		hlist_for_each_entry_safe(e, tmp, &t->bucket[i], hnode) {
			if (time_before(READ_ONCE(e->last_seen), limit)) {
				hlist_del_rcu(&e->hnode);
				kfree_rcu(e, rcu);
				atomic_dec(&t->count);
			}
		}
	}
	spin_unlock_bh(&t->lock);
}

unsigned int triade_localreg_count(struct triade_priv *triade)
{
	struct triade_local_table *t = triade->locals;

	return t ? (unsigned int)atomic_read(&t->count) : 0;
}

/* Walk the local table under RCU, invoking @cb for each entry. The callback
 * must not sleep and must not call back into the localreg.
 */
void triade_localreg_foreach(struct triade_priv *triade,
			     void (*cb)(const u8 *mac, unsigned long last_seen,
					void *ctx),
			     void *ctx)
{
	struct triade_local_table *t = triade->locals;
	struct triade_local *e;
	int i;

	if (!t)
		return;
	rcu_read_lock();
	for (i = 0; i < TRIADE_LOCAL_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(e, &t->bucket[i], hnode)
			cb(e->mac, e->last_seen, ctx);
	}
	rcu_read_unlock();
}
