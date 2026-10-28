/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

inline u64 rendering_scheduler_duration_microseconds(double duration_ms)
{
    auto microseconds = duration_ms * 1000.0;
    // NB: The exclusive upper bound is exactly representable, unlike u64's maximum value.
    //     Ordered comparisons also reject NaN and infinities before the integer conversion.
    if (!(microseconds > 0 && microseconds < 0x1p64))
        return 0;
    return static_cast<u64>(microseconds);
}

}
