/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Gfx {

enum class ExifOrientation {
    Default = 1,
    FlipHorizontally = 2,
    Rotate180 = 3,
    FlipVertically = 4,
    Rotate90ClockwiseThenFlipHorizontally = 5,
    Rotate90Clockwise = 6,
    FlipHorizontallyThenRotate90Clockwise = 7,
    Rotate90CounterClockwise = 8,
};

[[nodiscard]]
bool is_valid_exif_orientation(u32 orientation);

}
