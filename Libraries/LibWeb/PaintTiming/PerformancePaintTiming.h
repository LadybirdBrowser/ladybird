/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformancePaintTiming.h>
#include <LibWeb/PaintTiming/PaintTimingMixin.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PaintTiming {

// https://w3c.github.io/paint-timing/#sec-PerformancePaintTiming
class PerformancePaintTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_RUST_INTERFACE(PerformancePaintTiming, performance_paint_timing)

public:
    static GC::Ref<PerformancePaintTiming> create(Utf16String const& paint_type, PaintTimingInfo);
};

}
