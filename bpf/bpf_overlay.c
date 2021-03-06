// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2016-2020 Authors of Cilium */

#include <bpf/ctx/skb.h>
#include <bpf/api.h>

#include <node_config.h>
#include <netdev_config.h>

#include "lib/tailcall.h"
#include "lib/utils.h"
#include "lib/common.h"
#include "lib/maps.h"
#include "lib/ipv6.h"
#include "lib/eth.h"
#include "lib/dbg.h"
#include "lib/trace.h"
#include "lib/l3.h"
#include "lib/drop.h"
#include "lib/identity.h"
#include "lib/nodeport.h"

#ifdef ENABLE_IPV6
static __always_inline int handle_ipv6(struct __ctx_buff *ctx,
				       __u32 *identity)
{
	int ret, l3_off = ETH_HLEN, hdrlen;
	void *data_end, *data;
	struct ipv6hdr *ip6;
	struct bpf_tunnel_key key = {};
	struct endpoint_info *ep;
	bool decrypted;

	/* verifier workaround (dereference of modified ctx ptr) */
	if (!revalidate_data_first(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;
#ifdef ENABLE_NODEPORT
	if (!bpf_skip_nodeport(ctx)) {
		int ret = nodeport_lb6(ctx, *identity);
		if (ret < 0)
			return ret;
	}
#endif
	ret = encap_remap_v6_host_address(ctx, false);
	if (unlikely(ret < 0))
		return ret;

	if (!revalidate_data(ctx, &data, &data_end, &ip6))
		return DROP_INVALID;

	decrypted = ((ctx->mark & MARK_MAGIC_HOST_MASK) == MARK_MAGIC_DECRYPT);
	if (decrypted) {
		*identity = get_identity(ctx);
	} else {
		if (unlikely(ctx_get_tunnel_key(ctx, &key, sizeof(key), 0) < 0))
			return DROP_NO_TUNNEL_KEY;
		*identity = key.tunnel_id;

		/* Any node encapsulating will map any HOST_ID source to be
		 * presented as REMOTE_NODE_ID, therefore any attempt to signal
		 * HOST_ID as source from a remote node can be droppped. */
		if (*identity == HOST_ID) {
			return DROP_INVALID_IDENTITY;
		}
	}

	cilium_dbg(ctx, DBG_DECAP, key.tunnel_id, key.tunnel_label);

#ifdef ENABLE_IPSEC
	if (!decrypted) {
		/* IPSec is not currently enforce (feature coming soon)
		 * so for now just handle normally
		 */
		if (ip6->nexthdr != IPPROTO_ESP) {
			update_metrics(ctx->len, METRIC_INGRESS, REASON_PLAINTEXT);
			goto not_esp;
		}

		/* Decrypt "key" is determined by SPI */
		ctx->mark = MARK_MAGIC_DECRYPT;
		set_identity(ctx, key.tunnel_id);
		/* To IPSec stack on cilium_vxlan we are going to pass
		 * this up the stack but eth_type_trans has already labeled
		 * this as an OTHERHOST type packet. To avoid being dropped
		 * by IP stack before IPSec can be processed mark as a HOST
		 * packet.
		 */
		ctx_change_type(ctx, PACKET_HOST);
		return CTX_ACT_OK;
	} else {
		key.tunnel_id = get_identity(ctx);
		ctx->mark = 0;
	}
not_esp:
#endif

	/* Lookup IPv6 address in list of local endpoints */
	if ((ep = lookup_ip6_endpoint(ip6)) != NULL) {
		/* Let through packets to the node-ip so they are
		 * processed by the local ip stack */
		if (ep->flags & ENDPOINT_F_HOST)
			goto to_host;

		__u8 nexthdr = ip6->nexthdr;
		hdrlen = ipv6_hdrlen(ctx, l3_off, &nexthdr);
		if (hdrlen < 0)
			return hdrlen;

		return ipv6_local_delivery(ctx, l3_off, key.tunnel_id, ep, METRIC_INGRESS);
	}

to_host:
#ifdef HOST_IFINDEX
	if (1) {
		union macaddr host_mac = HOST_IFINDEX_MAC;
		union macaddr router_mac = NODE_MAC;
		int ret;

		ret = ipv6_l3(ctx, ETH_HLEN, (__u8 *) &router_mac.addr, (__u8 *) &host_mac.addr, METRIC_INGRESS);
		if (ret != CTX_ACT_OK)
			return ret;

		cilium_dbg_capture(ctx, DBG_CAPTURE_DELIVERY, HOST_IFINDEX);
		return redirect(HOST_IFINDEX, 0);
	}
#else
	return CTX_ACT_OK;
#endif
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV6_FROM_LXC) int tail_handle_ipv6(struct __ctx_buff *ctx)
{
	__u32 src_identity = 0;
	int ret = handle_ipv6(ctx, &src_identity);

	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, src_identity, ret, CTX_ACT_DROP, METRIC_INGRESS);

	return ret;
}
#endif /* ENABLE_IPV6 */

#ifdef ENABLE_IPV4
static __always_inline int handle_ipv4(struct __ctx_buff *ctx, __u32 *identity)
{
	void *data_end, *data;
	struct iphdr *ip4;
	struct endpoint_info *ep;
	struct bpf_tunnel_key key = {};
	bool decrypted;

	/* verifier workaround (dereference of modified ctx ptr) */
	if (!revalidate_data_first(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;
#ifdef ENABLE_NODEPORT
	if (!bpf_skip_nodeport(ctx)) {
		int ret = nodeport_lb4(ctx, *identity);
		if (ret < 0)
			return ret;
	}
#endif
	if (!revalidate_data(ctx, &data, &data_end, &ip4))
		return DROP_INVALID;

	decrypted = ((ctx->mark & MARK_MAGIC_HOST_MASK) == MARK_MAGIC_DECRYPT);
	/* If packets are decrypted the key has already been pushed into metadata. */
	if (decrypted) {
		*identity = get_identity(ctx);
	} else {
		if (unlikely(ctx_get_tunnel_key(ctx, &key, sizeof(key), 0) < 0))
			return DROP_NO_TUNNEL_KEY;
		*identity = key.tunnel_id;

		if (*identity == HOST_ID) {
			return DROP_INVALID_IDENTITY;
		}
	}

#ifdef ENABLE_IPSEC
	if (!decrypted) {
		/* IPSec is not currently enforce (feature coming soon)
		 * so for now just handle normally
		 */
		if (ip4->protocol != IPPROTO_ESP) {
			update_metrics(ctx->len, METRIC_INGRESS, REASON_PLAINTEXT);
			goto not_esp;
		}

		ctx->mark = MARK_MAGIC_DECRYPT;
		set_identity(ctx, key.tunnel_id);
		/* To IPSec stack on cilium_vxlan we are going to pass
		 * this up the stack but eth_type_trans has already labeled
		 * this as an OTHERHOST type packet. To avoid being dropped
		 * by IP stack before IPSec can be processed mark as a HOST
		 * packet.
		 */
		ctx_change_type(ctx, PACKET_HOST);
		return CTX_ACT_OK;
	} else {
		key.tunnel_id = get_identity(ctx);
		ctx->mark = 0;
	}
not_esp:
#endif

	/* Lookup IPv4 address in list of local endpoints */
	if ((ep = lookup_ip4_endpoint(ip4)) != NULL) {
		/* Let through packets to the node-ip so they are
		 * processed by the local ip stack */
		if (ep->flags & ENDPOINT_F_HOST)
			goto to_host;

		return ipv4_local_delivery(ctx, ETH_HLEN, key.tunnel_id, ip4, ep, METRIC_INGRESS);
	}

to_host:
#ifdef HOST_IFINDEX
	if (1) {
		union macaddr host_mac = HOST_IFINDEX_MAC;
		union macaddr router_mac = NODE_MAC;
		int ret;

		ret = ipv4_l3(ctx, ETH_HLEN, (__u8 *) &router_mac.addr, (__u8 *) &host_mac.addr, ip4);
		if (ret != CTX_ACT_OK)
			return ret;

		cilium_dbg_capture(ctx, DBG_CAPTURE_DELIVERY, HOST_IFINDEX);
		return redirect(HOST_IFINDEX, 0);
	}
#else
	return CTX_ACT_OK;
#endif
}

__section_tail(CILIUM_MAP_CALLS, CILIUM_CALL_IPV4_FROM_LXC) int tail_handle_ipv4(struct __ctx_buff *ctx)
{
	__u32 src_identity = 0;
	int ret = handle_ipv4(ctx, &src_identity);

	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, src_identity, ret, CTX_ACT_DROP, METRIC_INGRESS);

	return ret;
}
#endif /* ENABLE_IPV4 */

__section("from-overlay")
int from_overlay(struct __ctx_buff *ctx)
{
	__u16 proto;
	int ret;

	bpf_clear_cb(ctx);
	bpf_clear_nodeport(ctx);

	if (!validate_ethertype(ctx, &proto)) {
		/* Pass unknown traffic to the stack */
		ret = CTX_ACT_OK;
		goto out;
	}

#ifdef ENABLE_IPSEC
	if ((ctx->mark & MARK_MAGIC_HOST_MASK) == MARK_MAGIC_DECRYPT) {
		send_trace_notify(ctx, TRACE_FROM_OVERLAY, get_identity(ctx), 0, 0,
				  ctx->ingress_ifindex,
				  TRACE_REASON_ENCRYPTED, TRACE_PAYLOAD_LEN);
	} else
#endif
	{
		send_trace_notify(ctx, TRACE_FROM_OVERLAY, 0, 0, 0,
				  ctx->ingress_ifindex, 0, TRACE_PAYLOAD_LEN);
	}

	switch (proto) {
	case bpf_htons(ETH_P_IPV6):
#ifdef ENABLE_IPV6
		ep_tail_call(ctx, CILIUM_CALL_IPV6_FROM_LXC);
		ret = DROP_MISSED_TAIL_CALL;
#else
		ret = DROP_UNKNOWN_L3;
#endif
		break;

	case bpf_htons(ETH_P_IP):
#ifdef ENABLE_IPV4
		ep_tail_call(ctx, CILIUM_CALL_IPV4_FROM_LXC);
		ret = DROP_MISSED_TAIL_CALL;
#else
		ret = DROP_UNKNOWN_L3;
#endif
		break;

	default:
		/* Pass unknown traffic to the stack */
		ret = CTX_ACT_OK;
	}
out:
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, 0, ret, CTX_ACT_DROP, METRIC_INGRESS);
	return ret;
}

#ifdef ENABLE_NODEPORT
declare_tailcall_if(is_defined(ENABLE_IPV6), CILIUM_CALL_ENCAP_NODEPORT_NAT)
int tail_handle_nat_fwd(struct __ctx_buff *ctx)
{
	int ret;

	if ((ctx->mark & MARK_MAGIC_SNAT_DONE) == MARK_MAGIC_SNAT_DONE)
		return CTX_ACT_OK;
	ret = nodeport_nat_fwd(ctx, true);
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, 0, ret, CTX_ACT_DROP, METRIC_EGRESS);
	return ret;
}
#endif

__section("to-overlay")
int to_overlay(struct __ctx_buff *ctx)
{
	int ret = encap_remap_v6_host_address(ctx, true);
	if (unlikely(ret < 0))
		goto out;
#ifdef ENABLE_NODEPORT
	invoke_tailcall_if(is_defined(ENABLE_IPV6),
			   CILIUM_CALL_ENCAP_NODEPORT_NAT, tail_handle_nat_fwd);
#endif
out:
	if (IS_ERR(ret))
		return send_drop_notify_error(ctx, 0, ret, CTX_ACT_DROP, METRIC_EGRESS);
	return ret;
}

BPF_LICENSE("GPL");
