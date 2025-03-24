/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef TIME_CONVERTER_RTC_H
#define TIME_CONVERTER_RTC_H

#include <util/sys_vars.h>
#include "time_converter.h"

#ifdef DEFINED_IBV_CLOCK_INFO

class time_converter_rtc : public time_converter {
public:
    time_converter_rtc();
    virtual ~time_converter_rtc() {}

    inline void convert_hw_time_to_system_time(uint64_t hwtime, struct timespec *systime) override;
    void handle_timer_expired(void *) override;
};

#endif // DEFINED_IBV_CLOCK_INFO
#endif // TIME_CONVERTER_RTC_H
