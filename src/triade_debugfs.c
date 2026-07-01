// SPDX-License-Identifier: GPL-2.0
/*
 * Triade - debugfs exposition.
 *
 * Two read-only files per master, under /sys/kernel/debug/triade/<ifname>/:
 *
 *   stats   counters from struct triade_stats + node_count
 *   nodes   one line per learned ring neighbour: MAC, hop_via_port[2],
 *           last_seen age (ms).
 *
 * Useful for the lab test to assert that dedup, supervision and self-discard
 * are doing real work, not just that ping happens to succeed.
 */
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "triade.h"

static struct dentry *triade_debugfs_root;

#define S(field)	TRIADE_STAT_READ(triade, field)

static int triade_stats_show(struct seq_file *s, void *unused)
{
	struct triade_priv *triade = s->private;

	seq_printf(s, "rx_relayed         %llu\n", S(rx_relayed));
	seq_printf(s, "rx_self_discard    %llu\n", S(rx_self_discard));
	seq_printf(s, "rx_flood_relayed   %llu\n", S(rx_flood_relayed));
	seq_printf(s, "rx_super           %llu\n", S(rx_super));
	seq_printf(s, "rx_flood_dup       %llu\n", S(rx_flood_dup));
	seq_printf(s, "rx_local_deliver   %llu\n", S(rx_local_deliver));
	seq_printf(s, "tx_flood           %llu\n", S(tx_flood));
	seq_printf(s, "tx_spilled         %llu\n", S(tx_spilled));
	seq_printf(s, "node_aged_out      %llu\n", S(node_aged_out));
	seq_printf(s, "nports             %u\n", triade->nports);
	seq_printf(s, "node_count         %u\n",
		   triade_framereg_node_count(triade));
	seq_printf(s, "local_count        %u\n",
		   triade_localreg_count(triade));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(triade_stats);

struct nodes_ctx {
	struct seq_file *s;
};

static void node_cb(const u8 *mac, const u16 hop[TRIADE_MAX_PORTS],
		    unsigned long last_seen, void *ctx)
{
	struct nodes_ctx *nc = ctx;
	unsigned int age_ms = jiffies_to_msecs(jiffies - last_seen);
	char h0[8], h1[8];

	if (hop[0] == 0xFFFF)
		strscpy(h0, "-", sizeof(h0));
	else
		snprintf(h0, sizeof(h0), "%u", hop[0]);
	if (hop[1] == 0xFFFF)
		strscpy(h1, "-", sizeof(h1));
	else
		snprintf(h1, sizeof(h1), "%u", hop[1]);

	seq_printf(nc->s, "%pM  hops=[%s,%s]  age_ms=%u\n",
		   mac, h0, h1, age_ms);
}

static int triade_nodes_show(struct seq_file *s, void *unused)
{
	struct triade_priv *triade = s->private;
	struct nodes_ctx ctx = { .s = s };

	seq_puts(s, "# MAC                hops=[port0,port1]  age_ms\n");
	triade_framereg_foreach(triade, node_cb, &ctx);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(triade_nodes);

static void local_cb(const u8 *mac, unsigned long last_seen, void *ctx)
{
	struct seq_file *s = ctx;
	unsigned int age_ms = jiffies_to_msecs(jiffies - last_seen);

	seq_printf(s, "%pM  age_ms=%u\n", mac, age_ms);
}

/* The behind-bridge local-MAC set learned from TX egress (ADR-0006). Each
 * entry is a MAC this node delivers locally / self-discards, in addition to
 * the master address.
 */
static int triade_locals_show(struct seq_file *s, void *unused)
{
	struct triade_priv *triade = s->private;

	seq_puts(s, "# MAC                age_ms\n");
	triade_localreg_foreach(triade, local_cb, s);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(triade_locals);

static int triade_mode_get(void *data, u64 *val)
{
	struct triade_priv *triade = data;

	*val = READ_ONCE(triade->sched_mode);
	return 0;
}

static int triade_mode_set(void *data, u64 val)
{
	struct triade_priv *triade = data;

	if (val != TRIADE_SCHED_PER_FLOW &&
	    val != TRIADE_SCHED_PER_PACKET &&
	    val != TRIADE_SCHED_FLOW_ECMP)
		return -EINVAL;
	WRITE_ONCE(triade->sched_mode, (u8)val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(triade_mode_fops, triade_mode_get, triade_mode_set,
			 "%llu\n");

int triade_debugfs_module_init(void)
{
	triade_debugfs_root = debugfs_create_dir("triade", NULL);
	return 0;	/* debugfs failures are non-fatal */
}

void triade_debugfs_module_exit(void)
{
	debugfs_remove_recursive(triade_debugfs_root);
	triade_debugfs_root = NULL;
}

int triade_debugfs_add(struct triade_priv *triade)
{
	if (!triade_debugfs_root)
		return 0;
	triade->debugfs_dir = debugfs_create_dir(triade->dev->name,
						 triade_debugfs_root);
	debugfs_create_file("stats", 0444, triade->debugfs_dir, triade,
			    &triade_stats_fops);
	debugfs_create_file("nodes", 0444, triade->debugfs_dir, triade,
			    &triade_nodes_fops);
	debugfs_create_file("locals", 0444, triade->debugfs_dir, triade,
			    &triade_locals_fops);
	debugfs_create_file_unsafe("sched_mode", 0644, triade->debugfs_dir,
				   triade, &triade_mode_fops);
	return 0;
}

void triade_debugfs_remove(struct triade_priv *triade)
{
	debugfs_remove_recursive(triade->debugfs_dir);
	triade->debugfs_dir = NULL;
}
