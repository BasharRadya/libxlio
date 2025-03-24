/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef _DATA_UPDATER_H_
#define _DATA_UPDATER_H_

#include "core/proto/dst_entry.h"

class data_updater {
public:
    data_updater() {};
    virtual ~data_updater() = 0;
    virtual bool update_field(dst_entry &dst) = 0;
};

class header_ttl_hop_limit_updater : public data_updater {
public:
    header_ttl_hop_limit_updater(uint8_t ttl, bool is_multicast);
    virtual ~header_ttl_hop_limit_updater() {};
    virtual bool update_field(dst_entry &hdr);

private:
    uint8_t m_ttl_hop_limit;
    bool m_is_multicast;
};

class header_pcp_updater : public data_updater {
public:
    header_pcp_updater(uint8_t pcp);
    virtual ~header_pcp_updater() {};
    virtual bool update_field(dst_entry &hdr);

private:
    uint32_t m_pcp;
};

class header_tos_updater : public data_updater {
public:
    header_tos_updater(uint8_t pcp);
    virtual ~header_tos_updater() {};
    virtual bool update_field(dst_entry &hdr);

private:
    uint8_t m_tos;
};

class ring_alloc_logic_updater : public data_updater {
public:
    ring_alloc_logic_updater(int fd, lock_base &socket_lock,
                             resource_allocation_key &ring_alloc_logic,
                             socket_stats_t *socket_stats);
    virtual ~ring_alloc_logic_updater() {};
    virtual bool update_field(dst_entry &hdr);

private:
    int m_fd;
    lock_base &m_socket_lock;
    resource_allocation_key &m_key;
    socket_stats_t *m_sock_stats;
};
#endif /* _DATA_UPDATER_H_ */
