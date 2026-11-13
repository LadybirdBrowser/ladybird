/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::PaintTiming {

// https://w3c.github.io/paint-timing/#paint-timing-info
struct PaintTimingInfo {
    // https://w3c.github.io/paint-timing/#paint-timing-info-rendering-update-end-time
    HighResolutionTime::DOMHighResTimeStamp rendering_update_end_time { 0 };

    // https://w3c.github.io/paint-timing/#paint-timing-info-implementation-defined-presentation-time
    Optional<HighResolutionTime::DOMHighResTimeStamp> implementation_defined_presentation_time;
};

}
