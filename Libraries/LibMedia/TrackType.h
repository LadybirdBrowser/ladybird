/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media {

enum class TrackType : u8 {
    Video,
    Audio,
    Subtitles,
    Unknown,
};

}
