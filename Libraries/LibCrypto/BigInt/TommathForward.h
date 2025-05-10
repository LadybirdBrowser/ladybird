/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

extern "C" {
typedef uint64_t mp_digit;
typedef int mp_sign;

// This is a workaround for the fact that Tommath doesn't have a proper
// header file. It uses an anonymous struct to define the mp_int struct
// which makes it impossible to forward declare.
// Declare the mp_int struct with the same layout as the one in tommath.h
// and check that the layout is the same while avoiding a conflicting
// definition error (BN_H_ is defined in tommath.h).
// When importing this file it is important to always do it AFTER tommath.h.

typedef struct {
    int used, alloc;
    mp_sign sign;
    mp_digit* dp;
} mp_int_;

#ifndef BN_H_
typedef mp_int_ mp_int;
#else
static_assert(sizeof(mp_int_) == sizeof(mp_int));
static_assert(offsetof(mp_int_, used) == offsetof(mp_int, used));
static_assert(offsetof(mp_int_, alloc) == offsetof(mp_int, alloc));
static_assert(offsetof(mp_int_, sign) == offsetof(mp_int, sign));
static_assert(offsetof(mp_int_, dp) == offsetof(mp_int, dp));
#endif
}
