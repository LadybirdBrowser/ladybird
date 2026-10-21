/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGfx/Size.h>

namespace Compositing {

inline constexpr i64 max_canvas_area = 16384 * 16384;

inline constexpr bool is_valid_canvas_2d_size(Gfx::IntSize size)
{
    return !size.is_empty() && static_cast<i64>(size.width()) * static_cast<i64>(size.height()) <= max_canvas_area;
}

}
