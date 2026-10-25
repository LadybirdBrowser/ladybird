/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/PaintTiming/PerformancePaintTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::PaintTiming {

GC_DEFINE_ALLOCATOR(PerformancePaintTiming);

// https://w3c.github.io/paint-timing/#report-paint-timing
PerformancePaintTiming::PerformancePaintTiming(Utf16String const& paint_type, PaintTimingInfo paint_timing_info)
    // 1. Set newEntry's name attribute to paintType.
    // 3. Set newEntry's startTime attribute to the default paint timestamp given paintTimingInfo.
    // 4. Set newEntry's duration attribute to 0.
    : PerformanceTimeline::PerformanceEntry(paint_type, default_paint_timestamp(paint_timing_info), 0)
    // 2. Set newEntry's paint timing info to paintTimingInfo.
    , PaintTimingMixin(paint_timing_info)
{
}

PerformancePaintTiming::~PerformancePaintTiming() = default;

GC::Ref<PerformancePaintTiming> PerformancePaintTiming::create(Utf16String const& paint_type, PaintTimingInfo paint_timing_info)
{
    return GC::Heap::the().allocate<PerformancePaintTiming>(paint_type, paint_timing_info);
}

// https://w3c.github.io/paint-timing/#sec-PerformancePaintTiming
Utf16FlyString const& PerformancePaintTiming::entry_type() const
{
    // The entryType attribute's getter must return "paint".
    return PerformanceTimeline::EntryTypes::paint;
}

}
