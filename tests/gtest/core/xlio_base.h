/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef TESTS_GTEST_XLIO_BASE_H_
#define TESTS_GTEST_XLIO_BASE_H_

#include <xlio_extra.h>

/**
 * To enable xlio tests you need to set below EXTRA_API_ENABLED to 1
 * or you can add the following CPPFLAG during compilation 'make CPPFLAGS="-DEXTRA_API_ENABLED=1"'
 */
#ifndef EXTRA_API_ENABLED
#define EXTRA_API_ENABLED 0
#endif

/**
 * XLIO Base class for tests
 */
class xlio_base : virtual public testing::Test, virtual public test_base {
protected:
    virtual void SetUp();
    virtual void TearDown();

protected:
#if defined(EXTRA_API_ENABLED) && (EXTRA_API_ENABLED == 1)
    struct xlio_api_t *xlio_api;
#endif /* EXTRA_API_ENABLED */
};

#endif /* TESTS_GTEST_XLIO_BASE_H_ */
