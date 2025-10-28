/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Types.h>

namespace Media {

enum class FrameFlags : u8 {
    None = 0,
    Keyframe = 1 << 0,
};

}

AK_ENUM_BITWISE_OPERATORS(Media::FrameFlags);
