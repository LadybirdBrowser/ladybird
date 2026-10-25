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

// https://w3c.github.io/paint-timing/#default-paint-timestamp
inline HighResolutionTime::DOMHighResTimeStamp default_paint_timestamp(PaintTimingInfo const& paint_timing_info)
{
    // To get the default paint timestamp for a paint timing info paintTimingInfo, return paintTimingInfo's
    // implementation-defined presentation time if it is non-null, otherwise paintTimingInfo's rendering update end time.
    return paint_timing_info.implementation_defined_presentation_time.value_or(paint_timing_info.rendering_update_end_time);
}

// https://w3c.github.io/paint-timing/#sec-PaintTimingMixin
class PaintTimingMixin {
public:
    // https://w3c.github.io/paint-timing/#dom-painttimingmixin-painttime
    HighResolutionTime::DOMHighResTimeStamp paint_time() const
    {
        // The paintTime attribute's getter step is to return this's paint timing info's rendering update end time.
        return m_paint_timing_info.rendering_update_end_time;
    }

    // https://w3c.github.io/paint-timing/#dom-painttimingmixin-presentationtime
    Optional<HighResolutionTime::DOMHighResTimeStamp> presentation_time() const
    {
        // The presentationTime attribute's getter step, if exists, is to return this's paint timing info's
        // implementation-defined presentation time.
        return m_paint_timing_info.implementation_defined_presentation_time;
    }

    PaintTimingInfo const& paint_timing_info() const { return m_paint_timing_info; }

protected:
    explicit PaintTimingMixin(PaintTimingInfo paint_timing_info)
        : m_paint_timing_info(paint_timing_info)
    {
    }

private:
    // https://w3c.github.io/paint-timing/#painttimingmixin-paint-timing-info
    // NB: Every object that includes this mixin is created with a paint timing info, so it is never null here.
    PaintTimingInfo m_paint_timing_info;
};

}
