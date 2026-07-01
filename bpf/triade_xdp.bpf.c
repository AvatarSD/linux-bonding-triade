// SPDX-License-Identifier: GPL-2.0
/*
 * Triade XDP relay (M5).
 *
 * This program runs on each ring slave's RX path (native XDP on Intel NICs;
 * generic XDP on anything that doesn't speak it, including thunderbolt-net -
 * where the software fallback in the kernel module remains the real path).
 *
 * The relay's original job was to turn pass-through unicast into XDP_REDIRECT
 * to the peer port, sending everything else XDP_PASS so the kernel module owns
 * supervision, dedup, flood, local delivery and the local TX scheduler.
 *
 * BRIDGED-MASTER FIX (docs/adr/0006): the redirect fast path decided "local
 * vs pass-through" purely from `dst-MAC == my node MAC`. That is only valid
 * when the node's identity is the single master MAC. When triade0 is enslaved
 * into a bridge (Proxmox vmbr0 trunking VLANs across the ring), local delivery
 * targets are *behind-bridge* MACs (VM taps, per-VLAN SVIs) that are NOT the
 * master MAC - so the redirect wrongly bounced them back onto the ring
 * (black-hole) and never delivered them locally.
 *
 * XDP cannot see the kernel's learned local-MAC set without the deferred
 * kernel->map sync (ADR-0005 M5.5), so until that lands the redirect is
 * unsafe. This program now DEFERS every unicast to the kernel's software
 * forwarding plane, which learns local MACs from TX egress and does the
 * correct self-discard / local-deliver / pass-through decision. Only the two
 * MAC-invariant fast decisions stay in XDP.
 *
 * Decision tree, per RX frame on ingress port P:
 *
 *   ether_type == TRIADE_ETH_P_CTRL .. PASS  (kernel: supervision / flood)
 *   src-MAC    == my node MAC ........ DROP  (self-discard; circled the ring)
 *   otherwise ........................ PASS  (kernel: local-MAC-aware forward)
 *
 * The `triade_redirect` map + cfg.peer_ifindex are left populated by the
 * loader so the redirect can be re-enabled unchanged once the M5.5 sync feeds
 * XDP the local-MAC set (then: PASS if dst is local, REDIRECT peer otherwise).
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define TRIADE_ETH_P_CTRL 0x88B5

struct triade_xdp_cfg {
	__u8	mac[ETH_ALEN];
	__u8	_pad[2];
	__u32	peer_ifindex;
};

/* Per-ingress-port config: my node MAC + which peer to redirect to. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);			/* ingress ifindex */
	__type(value, struct triade_xdp_cfg);
} triade_config SEC(".maps");

/* Redirect targets, keyed by ifindex (BPF_REDIRECT_MAP looks this up). */
struct {
	__uint(type, BPF_MAP_TYPE_DEVMAP_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u32);
} triade_redirect SEC(".maps");

static __always_inline int mac_eq(const __u8 *a, const __u8 *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] &&
	       a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
}

SEC("xdp")
int triade_xdp_relay(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;
	__u32 ifindex = ctx->ingress_ifindex;
	struct ethhdr *eth = data;
	struct triade_xdp_cfg *cfg;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	cfg = bpf_map_lookup_elem(&triade_config, &ifindex);
	if (!cfg)
		return XDP_PASS;	/* not a Triade-managed port */

	if (eth->h_proto == bpf_htons(TRIADE_ETH_P_CTRL))
		return XDP_PASS;

	if (mac_eq(eth->h_source, cfg->mac))
		return XDP_DROP;	/* self-discard (pinned-MAC / corosync loop) */

	/* Bridged-master fix (ADR-0006): defer all remaining unicast/multicast
	 * to the kernel software plane, which knows the behind-bridge local-MAC
	 * set and does the correct self-discard / local-deliver / pass-through.
	 * The redirect fast path returns once XDP has the local set (M5.5).
	 */
	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
