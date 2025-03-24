/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef NETLINK_ROUTE_INFO_H_
#define NETLINK_ROUTE_INFO_H_

#include <netlink/route/rtnl.h>
#include <netlink/route/route.h>

#include "core/proto/route_val.h"

class netlink_route_info {
public:
    netlink_route_info(struct rtnl_route *nl_route_obj);
    ~netlink_route_info();

    const route_val &get_route_val() { return m_route_val; };

private:
    // fill all attributes using the provided netlink original route
    void fill(struct rtnl_route *nl_route_obj);

    route_val m_route_val;
};

#endif /* NETLINK_ROUTE_INFO_H_ */
