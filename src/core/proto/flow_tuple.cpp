/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#include "flow_tuple.h"
#include <core/util/vtypes.h>
#include <vlogger/vlogger.h>
#include <unordered_map>

#define MODULE_NAME "flow_tuple"

flow_tuple::flow_tuple()
    : m_dst_ip(INADDR_ANY)
    , m_src_ip(INADDR_ANY)
    , m_dst_port(INPORT_ANY)
    , m_src_port(INPORT_ANY)
    , m_protocol(PROTO_UNDEFINED)
    , m_family(AF_INET)
{
}

flow_tuple::flow_tuple(const ip_address &dst_ip, in_port_t dst_port, const ip_address &src_ip,
                       in_port_t src_port, in_protocol_t protocol, sa_family_t family)
    : m_dst_ip(dst_ip)
    , m_src_ip(src_ip)
    , m_dst_port(dst_port)
    , m_src_port(src_port)
    , m_protocol(protocol)
    , m_family(family)
{
}

flow_tuple::flow_tuple(const flow_tuple &ft)
{
    *this = ft;
}

flow_tuple::flow_tuple(flow_tuple &&ft)
{
    *this = std::move(ft);
}

flow_tuple &flow_tuple::operator=(const flow_tuple &ft)
{
    m_protocol = ft.m_protocol;
    m_dst_ip = ft.m_dst_ip;
    m_dst_port = ft.m_dst_port;
    m_src_ip = ft.m_src_ip;
    m_src_port = ft.m_src_port;
    m_family = ft.m_family;

    return *this;
}

flow_tuple &flow_tuple::operator=(flow_tuple &&ft)
{
    m_protocol = ft.m_protocol;
    m_dst_ip = ft.m_dst_ip;
    m_dst_port = ft.m_dst_port;
    m_src_ip = ft.m_src_ip;
    m_src_port = ft.m_src_port;
    m_family = ft.m_family;

    return *this;
}

bool flow_tuple::is_udp_uc() const
{
    return ((m_protocol == PROTO_UDP) && !m_dst_ip.is_mc(m_family));
}

bool flow_tuple::is_udp_mc() const
{
    return ((m_protocol == PROTO_UDP) && m_dst_ip.is_mc(m_family));
}

bool flow_tuple::is_5_tuple() const
{
    return (!m_src_ip.is_anyaddr() && m_src_port != INPORT_ANY);
}

bool flow_tuple::is_3_tuple() const
{
    return (m_src_ip.is_anyaddr() && m_src_port == INPORT_ANY);
}

size_t flow_tuple::hash() const
{
    std::hash<uint64_t> _hash;
    uint64_t val;

    const uint64_t *p_src_ip = reinterpret_cast<const uint64_t *>(&m_src_ip.get_in6_addr());
    const uint64_t *p_dst_ip = reinterpret_cast<const uint64_t *>(&m_dst_ip.get_in6_addr());

    val = (p_dst_ip[0] ^ p_dst_ip[1] ^ (static_cast<uint64_t>(m_dst_port) << 48ULL) ^ p_src_ip[0] ^
           p_src_ip[1] ^ (static_cast<uint64_t>(m_src_port) << 32ULL) ^
           (static_cast<uint64_t>(m_protocol) << 16ULL) ^ (static_cast<uint64_t>(m_family)));
    return _hash(val);
}

std::string flow_tuple::to_str() const
{
    std::string rc;
    rc.reserve(192);
    rc += "dst: ";
    rc += m_dst_ip.to_str(m_family);
    rc += ":";
    rc += std::to_string(ntohs(m_dst_port));
    rc += ", src: ";
    rc += m_src_ip.to_str(m_family);
    rc += ":";
    rc += std::to_string(ntohs(m_src_port));
    rc += ", proto: ";
    rc += __xlio_get_protocol_str(m_protocol);
    rc += ", family: ";
    rc += __xlio_get_family_str(m_family);

    return rc;
}

size_t flow_tuple_with_local_if::hash() const
{
    const uint64_t *p_ip = reinterpret_cast<const uint64_t *>(&m_local_if.get_in6_addr());
    return flow_tuple::hash() ^ p_ip[0] ^ p_ip[1];
}

std::string flow_tuple_with_local_if::to_str() const
{
    std::string rc;
    rc.reserve(192);
    rc += flow_tuple::to_str();
    rc += ", if: ";
    rc += m_local_if.to_str(m_family);
    return rc;
}
