/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>

#include <tommath.h>

inline ErrorOr<void> mp_error(mp_err error)
{
    switch (error) {
    case MP_OKAY:
        return {};
    case MP_MEM:
        return Error::from_errno(ENOMEM);
    case MP_VAL:
        return Error::from_errno(EINVAL);
    case MP_ITER:
        return Error::from_string_literal("Maximum iterations reached");
    case MP_BUF:
        return Error::from_string_literal("Buffer overflow");
    default:
        return Error::from_string_literal("Unknown error");
    }
}

#define MP_TRY(...) TRY(mp_error((__VA_ARGS__)))

#define MP_MUST(...) MUST(mp_error((__VA_ARGS__)))
