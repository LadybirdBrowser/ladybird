/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Types.h>

namespace Audio {

// Whether the AudioServer has recently heard something, muted or not.
enum class Audibility : u8 {
    Inaudible,
    Audible,
};

// How long a group goes on counting as audible after its sound stops, so a pause between sounds isn't reported as one
// sound ending and another starting.
static constexpr AK::Duration AUDIBILITY_SMOOTHING = AK::Duration::from_milliseconds(500);

}
