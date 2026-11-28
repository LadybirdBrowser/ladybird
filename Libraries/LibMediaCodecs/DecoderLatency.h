/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media {

// Output latency policy, fixed for the decoder's lifetime.
enum class DecoderLatency : u8 {
    // Frames may come out only once the ones after them are under way, as when several are decoded at once.
    Default,
    // Each frame comes out before the next one goes in, for media that is shown as it arrives.
    Low,
};

}
