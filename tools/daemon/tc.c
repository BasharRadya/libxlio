/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hash.h"
#include "tc.h"
#include "daemon.h"
#include "nl.h"

/* Traffic control usage method
 * 0 - tc application
 * 1 - netlink api
 */
#define USE_NETLINK 1

/**
 * @struct tc_object
 * @brief tc container
 */
struct tc_object {
    nl_t nl; /**< netlink object */
    struct nl_req req; /**< netlink request storage */
};

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
/* Use iproute2 / tc implementation as a reference
 * to pack data for specific attribute
 */
static int pack_key(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask);
static int pack_key8(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask);
static int pack_key16(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask);
static int pack_key32(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask);
static int pack_ip6(struct tc_u32_sel *sel, uint8_t *addr, uint32_t mask, int off, int offmask);
#endif /* USE_NETLINK */

tc_t tc_create(void)
{
    tc_t tc = NULL;

    tc = (struct tc_object *)malloc(sizeof(*tc));
    if (tc) {
        tc->nl = nl_create();
        if (NULL == tc->nl) {
            log_error("Unable to create a netlink object\n");
            goto err;
        }
        memset(&tc->req, 0, sizeof(tc->req));
    }

    return tc;
err:
    free(tc);
    tc = NULL;

    return NULL;
}

void tc_destroy(tc_t tc)
{
    if (tc) {
        nl_destroy(tc->nl);
        free(tc);
        tc = NULL;
    }
}

void tc_req(tc_t tc, int ifindex, uint16_t proto, uint16_t type, uint16_t flags,
            struct tc_qdisc qdisc)
{
    memset(&tc->req, 0, sizeof(tc->req));

    tc->req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(tc->req.msg));
    tc->req.hdr.nlmsg_type = type;
    tc->req.hdr.nlmsg_flags = (flags ? flags : (NLM_F_REQUEST | NLM_F_ACK));
    tc->req.hdr.nlmsg_pid = 0; /* to communicate kernel */
    tc->req.hdr.nlmsg_seq = 0; /* update during send */

    tc->req.msg.tcm_family = AF_UNSPEC;
    tc->req.msg.tcm_ifindex = ifindex;
    tc->req.msg.tcm_handle = qdisc.handle;
    tc->req.msg.tcm_parent = qdisc.parent;
    tc->req.msg.tcm_info = TC_H_MAKE(qdisc.prio << 16, htons(proto));
}

int tc_add_qdisc(tc_t tc, int ifindex)
{
    int rc = 0;

    log_debug("add qdisc using if_id: %d\n", ifindex);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {TC_H_MAKE(TC_H_INGRESS, 0), TC_H_INGRESS, 0};
    struct rtattr *opts = NULL;

    tc_req(tc, ifindex, 0, RTM_NEWQDISC, (NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE),
           qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, "ingress", sizeof("ingress"));

    opts = nl_attr_nest_start(&tc->req.hdr, TCA_OPTIONS);
    nl_attr_nest_end(&tc->req.hdr, opts);

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if ((nl_recv(tc->nl, NULL, NULL) < 0) && (errno != EEXIST)) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex, if_name)) {
        rc = -errno;
        goto err;
    }

    out_buf = sys_exec("tc qdisc add dev %s handle ffff: ingress "
                       "> /dev/null 2>&1 || echo $?",
                       if_name);
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

int tc_del_qdisc(tc_t tc, int ifindex)
{
    int rc = 0;

    log_debug("remove qdisc using if_id: %d\n", ifindex);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {TC_H_MAKE(TC_H_INGRESS, 0), TC_H_INGRESS, 0};
    struct rtattr *opts = NULL;

    tc_req(tc, ifindex, 0, RTM_DELQDISC, 0, qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, "ingress", sizeof("ingress"));

    opts = nl_attr_nest_start(&tc->req.hdr, TCA_OPTIONS);
    nl_attr_nest_end(&tc->req.hdr, opts);

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if ((nl_recv(tc->nl, NULL, NULL) < 0) && (errno != EINVAL)) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex, if_name)) {
        rc = -errno;
        goto err;
    }

    out_buf = sys_exec("tc qdisc del dev %s handle ffff: ingress "
                       "> /dev/null 2>&1 || echo $?",
                       if_name);
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

int tc_add_filter_divisor(tc_t tc, int ifindex, int prio, int ht, uint16_t proto)
{
    int rc = 0;

    log_debug("apply filter divisor using if_id: %d proto: %04hx\n", ifindex, proto);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {HANDLE_SET(ht, 0, 0), 0xffff0000, prio};
    char opt_kind[] = "u32";
    uint32_t opt_divisor = 256;
    struct rtattr *opts = NULL;

    tc_req(tc, ifindex, proto, RTM_NEWTFILTER,
           (NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE), qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, opt_kind, sizeof(opt_kind));

    opts = nl_attr_nest_start(&tc->req.hdr, TCA_OPTIONS);
    nl_attr_add(&tc->req.hdr, TCA_U32_DIVISOR, &opt_divisor, sizeof(opt_divisor));
    nl_attr_nest_end(&tc->req.hdr, opts);

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if (nl_recv(tc->nl, NULL, NULL) < 0) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex, if_name)) {
        rc = -errno;
        goto err;
    }

    out_buf =
        sys_exec("tc filter add dev %s parent ffff: prio %d handle %x: protocol %s u32 divisor 256 "
                 "> /dev/null 2>&1 || echo $?",
                 if_name, prio, ht, (proto == ETH_P_IP ? "ip" : "ipv6"));
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

int tc_add_filter_link(tc_t tc, int ifindex, int prio, int ht, int id, struct sockaddr_store *ip)
{
    int rc = 0;
    uint16_t proto = (ip->family == AF_INET ? ETH_P_IP : ETH_P_IPV6);

    log_debug("add link filter using if_id: %d\n", ifindex);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {HANDLE_SET(0, 0, id), 0xffff0000, prio};
    char opt_kind[] = "u32";
    uint32_t opt_link = HANDLE_SET(id, 0, 0);
    uint32_t opt_ht = HANDLE_SET(ht, 0, 0);
    struct rtattr *opts = NULL;
    struct {
        union {
            struct tc_u32_sel sel;
            uint8_t pad[sizeof(struct tc_u32_sel) + sizeof(struct tc_u32_key) * 20U];
        };
    } opt_sel;

    tc_req(tc, ifindex, proto, RTM_NEWTFILTER,
           (NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE), qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, opt_kind, sizeof(opt_kind));

    opts = nl_attr_nest_start(&tc->req.hdr, TCA_OPTIONS);
    nl_attr_add(&tc->req.hdr, TCA_U32_LINK, &opt_link, sizeof(opt_link));
    nl_attr_add(&tc->req.hdr, TCA_U32_HASH, &opt_ht, sizeof(opt_ht));
    memset(&opt_sel, 0, sizeof(opt_sel));
    /* hashkey option:
     * mask: 0x000000ff
     * at: 20
     */
    opt_sel.sel.hmask = htonl(0x000000ff);
    opt_sel.sel.hoff = 20;
    /* match option for ip protocol:
     * dst: 16
     * addr/mask: ip/0xffffffff
     */
    if (proto == ETH_P_IP) {
        pack_key32(&opt_sel.sel, ntohl(ip->addr4.sin_addr.s_addr), 0xffffffff, 16, 0);
    } else {
        pack_ip6(&opt_sel.sel, ip->addr6.sin6_addr.s6_addr, 0xffffffff, 16, 0);
    }
    nl_attr_add(&tc->req.hdr, TCA_U32_SEL, &opt_sel,
                sizeof(opt_sel.sel) + opt_sel.sel.nkeys * sizeof(opt_sel.sel.keys[0]));
    nl_attr_nest_end(&tc->req.hdr, opts);

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if (nl_recv(tc->nl, NULL, NULL) < 0) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex, if_name)) {
        rc = -errno;
        goto err;
    }

    out_buf = sys_exec("tc filter add dev %s protocol %s parent ffff: prio %d handle ::%x u32 "
                       "ht %x:: match ip dst %s hashkey mask 0x000000ff at 20 link %x: "
                       "> /dev/null 2>&1 || echo $?",
                       if_name, (proto == ETH_P_IP ? "ip" : "ipv6"), prio, id, ht,
                       (proto == ETH_P_IP ? "ip" : "ip6"), sys_ip2str(ip), id);
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

int tc_add_filter_tap2dev(tc_t tc, int ifindex, int prio, int id, uint16_t proto,
                          struct sockaddr_store *ip, int ifindex_to)
{
    int rc = 0;

    proto = (proto == AF_INET ? ETH_P_IP : ETH_P_IPV6);
    log_debug("add filter to redirect traffic from if_id: %d to if_id: %d\n", ifindex, ifindex_to);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {HANDLE_SET(0, 0, id), 0xffff0000, prio};
    char opt_kind[] = "u32";
    uint32_t opt_ht = HANDLE_SET(0x800, 0, 0);
    struct rtattr *opts = NULL;
    struct {
        union {
            struct tc_u32_sel sel;
            uint8_t pad[sizeof(struct tc_u32_sel) + sizeof(struct tc_u32_key) * 20U];
        };
    } opt_sel;

    tc_req(tc, ifindex, proto, RTM_NEWTFILTER,
           (NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE), qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, opt_kind, sizeof(opt_kind));

    /* [filter] options filling */
    opts = nl_attr_nest_start(&tc->req.hdr, TCA_OPTIONS);
    {
        struct rtattr *opts_action = NULL;

        /* [action] options filling */
        opts_action = nl_attr_nest_start(&tc->req.hdr, TCA_U32_ACT);
        {
            int opt_prio = 0;
            char opt_act_kind[] = "mirred";
            struct rtattr *opts_action_prio = NULL;

            /* [mirred] options filling */
            opts_action_prio = nl_attr_nest_start(&tc->req.hdr, ++opt_prio);
            nl_attr_add(&tc->req.hdr, TCA_ACT_KIND, opt_act_kind, sizeof(opt_act_kind));
            {
                struct rtattr *opts_action_prio_mirred = NULL;
                struct tc_mirred opt_mirred;

                opts_action_prio_mirred = nl_attr_nest_start(&tc->req.hdr, TCA_ACT_OPTIONS);
                memset(&opt_mirred, 0, sizeof(opt_mirred));
                opt_mirred.eaction = TCA_EGRESS_REDIR;
                opt_mirred.action = TC_ACT_STOLEN;
                opt_mirred.ifindex = ifindex_to;
                nl_attr_add(&tc->req.hdr, TCA_MIRRED_PARMS, &opt_mirred, sizeof(opt_mirred));

                nl_attr_nest_end(&tc->req.hdr, opts_action_prio_mirred);
            }

            nl_attr_nest_end(&tc->req.hdr, opts_action_prio);
        }

        nl_attr_nest_end(&tc->req.hdr, opts_action);
    }

    nl_attr_add(&tc->req.hdr, TCA_U32_HASH, &opt_ht, sizeof(opt_ht));
    memset(&opt_sel, 0, sizeof(opt_sel));
    /* match option for ip protocol:
     * dst: 16
     * addr/mask: addr/0xffffffff
     */
    if (ip) {
        if (proto == ETH_P_IP) {
            pack_key32(&opt_sel.sel, ntohl(ip->addr4.sin_addr.s_addr), 0xffffffff, 16, 0);
        } else {
            pack_ip6(&opt_sel.sel, ip->addr6.sin6_addr.s6_addr, 0xffffffff, 16, 0);
        }
    } else {
        if (proto == ETH_P_IP) {
            pack_key32(&opt_sel.sel, ntohl(0), 0, 0, 0);
        } else {
            uint32_t s_addr[4] = {0, 0, 0, 0};
            pack_ip6(&opt_sel.sel, (uint8_t *)s_addr, 0xffffffff, 0, 0);
        }
    }
    opt_sel.sel.flags |= TC_U32_TERMINAL;
    nl_attr_add(&tc->req.hdr, TCA_U32_SEL, &opt_sel,
                sizeof(opt_sel.sel) + opt_sel.sel.nkeys * sizeof(opt_sel.sel.keys[0]));

    nl_attr_nest_end(&tc->req.hdr, opts);

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if (nl_recv(tc->nl, NULL, NULL) < 0) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];
    char tap_name[IF_NAMESIZE];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex_to, if_name)) {
        rc = -errno;
        goto err;
    }

    if (NULL == if_indextoname(ifindex, tap_name)) {
        rc = -errno;
        goto err;
    }

    if (ip) {
        out_buf = sys_exec("tc filter add dev %s protocol %s parent ffff: prio %d "
                           "handle ::%d u32 ht 800:: "
                           "match %s dst %s action mirred egress redirect dev %s "
                           "> /dev/null 2>&1 || echo $?",
                           tap_name, (proto == ETH_P_IP ? "ip" : "ipv6"), prio, id,
                           (proto == ETH_P_IP ? "ip" : "ip6"), sys_ip2str(ip), if_name);
    } else {
        out_buf = sys_exec("tc filter add dev %s protocol %s parent ffff: prio %d "
                           "handle ::%d u32 ht 800:: "
                           "match u8 0 0 action mirred egress redirect dev %s "
                           "> /dev/null 2>&1 || echo $?",
                           tap_name, (proto == ETH_P_IP ? "ip" : "ipv6"), prio, id, if_name);
    }
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

int tc_add_filter_dev2tap(tc_t tc, int ifindex, int prio, int ht, int bkt, int id, int l4_proto,
                          struct sockaddr_store *dst_ip, struct sockaddr_store *src_ip,
                          int ifindex_to)
{
    int rc = 0;
    uint16_t proto = (dst_ip->family == AF_INET ? ETH_P_IP : ETH_P_IPV6);
    uint16_t dst_port =
        (dst_ip->family == AF_INET ? dst_ip->addr4.sin_port : dst_ip->addr6.sin6_port);
    uint16_t src_port =
        (src_ip ? (src_ip->family == AF_INET ? src_ip->addr4.sin_port : src_ip->addr6.sin6_port)
                : 0);

    log_debug("add filter to redirect traffic from if_id: %d to if_id: %d\n", ifindex, ifindex_to);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {HANDLE_SET(0, 0, id), 0xffff0000, prio};
    char opt_kind[] = "u32";
    uint32_t opt_ht = HANDLE_SET(ht, bkt, 0);
    struct rtattr *opts = NULL;
    struct {
        union {
            struct tc_u32_sel sel;
            uint8_t pad[sizeof(struct tc_u32_sel) + sizeof(struct tc_u32_key) * 10U];
        };
    } opt_sel;

    tc_req(tc, ifindex, proto, RTM_NEWTFILTER,
           (NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE), qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, opt_kind, sizeof(opt_kind));

    /* [filter] options filling */
    opts = nl_attr_nest_start(&tc->req.hdr, TCA_OPTIONS);
    {
        struct rtattr *opts_action = NULL;

        /* [action] options filling */
        opts_action = nl_attr_nest_start(&tc->req.hdr, TCA_U32_ACT);
        {
            int opt_prio = 0;
            char opt_act_kind[] = "mirred";
            struct rtattr *opts_action_prio = NULL;

            /* [mirred] options filling */
            opts_action_prio = nl_attr_nest_start(&tc->req.hdr, ++opt_prio);
            nl_attr_add(&tc->req.hdr, TCA_ACT_KIND, opt_act_kind, sizeof(opt_act_kind));
            {
                struct rtattr *opts_action_prio_mirred = NULL;
                struct tc_mirred opt_mirred;

                opts_action_prio_mirred = nl_attr_nest_start(&tc->req.hdr, TCA_ACT_OPTIONS);
                memset(&opt_mirred, 0, sizeof(opt_mirred));
                opt_mirred.eaction = TCA_EGRESS_REDIR;
                opt_mirred.action = TC_ACT_STOLEN;
                opt_mirred.ifindex = ifindex_to;
                nl_attr_add(&tc->req.hdr, TCA_MIRRED_PARMS, &opt_mirred, sizeof(opt_mirred));

                nl_attr_nest_end(&tc->req.hdr, opts_action_prio_mirred);
            }

            nl_attr_nest_end(&tc->req.hdr, opts_action_prio);
        }

        nl_attr_nest_end(&tc->req.hdr, opts_action);
    }

    nl_attr_add(&tc->req.hdr, TCA_U32_HASH, &opt_ht, sizeof(opt_ht));
    memset(&opt_sel, 0, sizeof(opt_sel));
    /* [match] protocol option */
    pack_key8(&opt_sel.sel, l4_proto, 0xff, 9, 0);
    /* [match] nofrag option */
    pack_key16(&opt_sel.sel, 0, 0x3fff, 6, 0);
    if (src_ip) {
        /* [match] src option */
        if (proto == ETH_P_IP) {
            pack_key32(&opt_sel.sel, ntohl(src_ip->addr4.sin_addr.s_addr), 0xffffffff, 12, 0);
        } else {
            pack_ip6(&opt_sel.sel, src_ip->addr6.sin6_addr.s6_addr, 0xffffffff, 12, 0);
        }
        /* [match] sport option */
        pack_key16(&opt_sel.sel, ntohs(src_port), 0xffff, 20, 0);
    }
    /* [match] dst option */
    if (proto == ETH_P_IP) {
        pack_key32(&opt_sel.sel, ntohl(dst_ip->addr4.sin_addr.s_addr), 0xffffffff, 16, 0);
    } else {
        pack_ip6(&opt_sel.sel, dst_ip->addr6.sin6_addr.s6_addr, 0xffffffff, 16, 0);
    }
    /* [match] dport option */
    pack_key16(&opt_sel.sel, ntohs(dst_port), 0xffff, 22, 0);
    opt_sel.sel.flags |= TC_U32_TERMINAL;
    nl_attr_add(&tc->req.hdr, TCA_U32_SEL, &opt_sel,
                sizeof(opt_sel.sel) + opt_sel.sel.nkeys * sizeof(opt_sel.sel.keys[0]));

    nl_attr_nest_end(&tc->req.hdr, opts);

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if (nl_recv(tc->nl, NULL, NULL) < 0) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];
    char tap_name[IF_NAMESIZE];
    char str_tmp[100];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex, if_name)) {
        rc = -errno;
        goto err;
    }

    if (NULL == if_indextoname(ifindex_to, tap_name)) {
        rc = -errno;
        goto err;
    }

    if (src_ip) {
        strncpy(str_tmp, sys_ip2str(src_ip), sizeof(str_tmp));
        str_tmp[sizeof(str_tmp) - 1] = '\0';
        out_buf = sys_exec("tc filter add dev %s parent ffff: protocol %s "
                           "prio %d handle ::%x u32 ht %x:%x: "
                           "match %s protocol %d 0xff "
                           "match %s nofrag "
                           "match %s src %s match ip sport %d 0xffff "
                           "match %s dst %s match ip dport %d 0xffff "
                           "action mirred egress redirect dev %s "
                           "> /dev/null 2>&1 || echo $?",
                           if_name, (proto == ETH_P_IP ? "ip" : "ipv6"), prio, id, ht, bkt,
                           (proto == ETH_P_IP ? "ip" : "ip6"), l4_proto,
                           (proto == ETH_P_IP ? "ip" : "ip6"), (proto == ETH_P_IP ? "ip" : "ip6"),
                           str_tmp, src_port, (proto == ETH_P_IP ? "ip" : "ip6"),
                           sys_ip2str(dst_ip), ntohs(dst_port), tap_name);
    } else {
        out_buf = sys_exec("tc filter add dev %s parent ffff: protocol %s "
                           "prio %d handle ::%x u32 ht %x:%x: "
                           "match %s protocol %d 0xff "
                           "match %s nofrag "
                           "match %s dst %s match ip dport %d 0xffff "
                           "action mirred egress redirect dev %s "
                           "> /dev/null 2>&1 || echo $?",
                           if_name, (proto == ETH_P_IP ? "ip" : "ipv6"), prio, id, ht, bkt,
                           (proto == ETH_P_IP ? "ip" : "ip6"), l4_proto,
                           (proto == ETH_P_IP ? "ip" : "ip6"), (proto == ETH_P_IP ? "ip" : "ip6"),
                           sys_ip2str(dst_ip), ntohs(dst_port), tap_name);
    }
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

int tc_del_filter(tc_t tc, int ifindex, int prio, int ht, int bkt, int id, uint16_t proto)
{
    int rc = 0;

    log_debug("remove filter for if_id: %d proto: %04hx\n", ifindex, proto);

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
    struct tc_qdisc qdisc = {HANDLE_SET(ht, bkt, id), 0xffff0000, prio};
    char opt_kind[] = "u32";

    tc_req(tc, ifindex, proto, RTM_DELTFILTER, 0, qdisc);

    nl_attr_add(&tc->req.hdr, TCA_KIND, opt_kind, sizeof(opt_kind));

    if (nl_send(tc->nl, &tc->req.hdr) < 0) {
        rc = -1;
        goto err;
    }
    if (nl_recv(tc->nl, NULL, NULL) < 0) {
        rc = -1;
        goto err;
    }
#else
    char *out_buf = NULL;
    char if_name[IF_NAMESIZE];

    NOT_IN_USE(tc);

    if (NULL == if_indextoname(ifindex, if_name)) {
        rc = -errno;
        goto err;
    }

    out_buf = sys_exec("tc filter del dev %s parent ffff: protocol %s prio %d handle %x:%x:%x u32 "
                       "> /dev/null 2>&1 || echo $?",
                       if_name, (proto == ETH_P_IP ? "ip" : "ipv6"), prio, ht, bkt, id);
    if (NULL == out_buf || (out_buf[0] != '\0' && out_buf[0] != '0')) {
        rc = -1;
        goto err;
    }
#endif /* USE_NETLINK */

err:
    return rc;
}

#if defined(USE_NETLINK) && (USE_NETLINK == 1)
static int pack_key(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask)
{
    int i;

    key &= mask;

    for (i = 0; i < sel->nkeys; i++) {
        if ((sel->keys[i].off == off) && (sel->keys[i].offmask == offmask)) {
            uint32_t intersect = mask & sel->keys[i].mask;

            if ((key ^ sel->keys[i].val) & intersect) {
                return -1;
            }
            sel->keys[i].val |= key;
            sel->keys[i].mask |= mask;
            return 0;
        }
    }

    if (off % 4) {
        return -1;
    }
    sel->keys[sel->nkeys].val = key;
    sel->keys[sel->nkeys].mask = mask;
    sel->keys[sel->nkeys].off = off;
    sel->keys[sel->nkeys].offmask = offmask;
    sel->nkeys++;

    return 0;
}

static int pack_key8(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask)
{
    if ((off & 3) == 0) {
        key <<= 24;
        mask <<= 24;
    } else if ((off & 3) == 1) {
        key <<= 16;
        mask <<= 16;
    } else if ((off & 3) == 2) {
        key <<= 8;
        mask <<= 8;
    }
    off &= ~3;
    key = htonl(key);
    mask = htonl(mask);

    return pack_key(sel, key, mask, off, offmask);
}

static int pack_key16(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask)
{
    if ((off & 3) == 0) {
        key <<= 16;
        mask <<= 16;
    }
    off &= ~3;
    key = htonl(key);
    mask = htonl(mask);

    return pack_key(sel, key, mask, off, offmask);
}

static int pack_key32(struct tc_u32_sel *sel, uint32_t key, uint32_t mask, int off, int offmask)
{
    key = htonl(key);
    mask = htonl(mask);

    return pack_key(sel, key, mask, off, offmask);
}

static int pack_ip6(struct tc_u32_sel *sel, uint8_t *addr, uint32_t mask, int off, int offmask)
{
    int ret = 0;
    int i = 0;
    uint32_t key = 0;

    for (i = 0; i < 4; i++) {
        key = htonl(((uint32_t *)addr)[i]);
        mask = htonl(mask);
        ret = pack_key(sel, key, mask, off + 4 * (i - 1), offmask);
        if (ret) {
            return ret;
        }
    }

    return ret;
}
#endif /* USE_NETLINK */
