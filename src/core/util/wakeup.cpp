/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#include "utils/bullseye.h"
#include "vlogger/vlogger.h"
#include "wakeup.h"
#include <core/sock/sock-redirect.h>

#define MODULE_NAME "wakeup"

#define wkup_logpanic   __log_info_panic
#define wkup_logerr     __log_info_err
#define wkup_logwarn    __log_info_warn
#define wkup_loginfo    __log_info_info
#define wkup_logdbg     __log_info_dbg
#define wkup_logfunc    __log_info_func
#define wkup_logfuncall __log_info_funcall
#define wkup_entry_dbg  __log_entry_dbg

#undef MODULE_HDR_INFO
#define MODULE_HDR_INFO MODULE_NAME "[epfd=%d]:%d:%s() "
#undef __INFO__
#define __INFO__ m_wakeup_epfd

wakeup::wakeup()
{
    m_wakeup_epfd = 0;
    m_is_sleeping = 0;
    memset(&m_ev, 0, sizeof(m_ev));
}
void wakeup::going_to_sleep()
{
    BULLSEYE_EXCLUDE_BLOCK_START
    if (likely(m_wakeup_epfd)) {
        m_is_sleeping++;
    } else {
        wkup_logerr(" m_wakeup_epfd is not initialized - cannot use wakeup mechanism\n");
        m_is_sleeping = 0;
    }
    BULLSEYE_EXCLUDE_BLOCK_END
}

void wakeup::wakeup_set_epoll_fd(int epfd)
{
    m_wakeup_epfd = epfd;
}
