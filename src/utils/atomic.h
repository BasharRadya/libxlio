/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef ATOMIC_H_
#define ATOMIC_H_

#include "asm.h"
#include "utils/bullseye.h"

struct atomic_t {
    __volatile__ int counter;
};

#define ATOMIC_INIT(i)                                                                             \
    {                                                                                              \
        (i)                                                                                        \
    }

/**
 * Read atomic variable.
 * @param v pointer of type atomic_t
 * @return Value of the atomic.
 *
 * Atomically reads the value of @v.
 */
#define atomic_read(v) ((v)->counter)

/**
 * Set atomic variable.
 * @param v pointer of type atomic_t.
 * @param i required value.
 */
#define atomic_set(v, i) (((v)->counter) = (i))

#if 0

#if _BullseyeCoverage
#pragma BullseyeCoverage off
#endif

/**
 *  Returns current contents of addr and replaces contents with value.
 *  @param value Values to set.
 *  @param addr Address to set.
 *  @return Previous value of *addr.
 */
template<typename T>
static inline T atomic_swap(T new_value, T *addr)
{
	return (T)xchg((unsigned long)new_value, (void*)addr);
}

/**
 *  Replaces *addr with new_value if it equals old_value.
 *  @param old_value Expected value.
 *  @param new_value Value to set.
 *  @param addr Address to set.
 *  @return true if was set, false if not.
 */
template<typename T>
static bool atomic_cas(T old_value, T new_value, T *addr)
{
	return cmpxchg((unsigned long)old_value, (unsigned long)new_value, (void*)addr);
}
#if _BullseyeCoverage
#pragma BullseyeCoverage on
#endif

#endif

/**
 * Add to the atomic variable.
 * @param v pointer of type atomic_t.
 * @return Value before add.
 */
static inline int atomic_fetch_and_inc(atomic_t *v)
{
    return atomic_fetch_and_add(1, &v->counter);
}

/**
 * Add to the atomic variable.
 * @param v pointer of type atomic_t.
 * @return Value before add.
 */
static inline int atomic_fetch_and_dec(atomic_t *v)
{
    return atomic_fetch_and_add(-1, &v->counter);
}

/**
 * Add to the atomic variable.
 * @param x integer value to add.
 * @param v pointer of type atomic_t.
 * @return Value before add.
 */
static inline int atomic_fetch_add_relaxed(int x, atomic_t *v)
{
    return atomic_fetch_and_add_relaxed(x, &v->counter);
}

#endif /* ATOMIC_H_ */
