/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media {

// Whether a video sink follows its clock, which its owner decides once the sink is reserved.
enum class VideoSinkTicking : u8 {
    Undecided,
    Yes,
    No,
};

}
