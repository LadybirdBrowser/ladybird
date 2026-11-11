/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibGfx/Rect.h>

namespace Compositing {

constexpr i64 caret_blink_interval_ns = 500'000'000;
inline bool caret_is_visible_at_time(PaintCaret const& caret, i64 monotonic_time_ns)
{
    if (!caret.should_blink)
        return true;
    auto elapsed_ns = monotonic_time_ns > caret.blink_cycle_start_time_ns
        ? monotonic_time_ns - caret.blink_cycle_start_time_ns
        : 0;
    return (elapsed_ns / caret_blink_interval_ns) % 2 == 0;
}

}
