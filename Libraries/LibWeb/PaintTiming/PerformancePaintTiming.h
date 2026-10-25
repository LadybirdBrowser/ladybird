/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PaintTiming/PaintTimingMixin.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PaintTiming {

// https://w3c.github.io/paint-timing/#sec-PerformancePaintTiming
class PerformancePaintTiming final
    : public PerformanceTimeline::PerformanceEntry
    , public PaintTimingMixin {
    WEB_WRAPPABLE(PerformancePaintTiming, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformancePaintTiming);

public:
    virtual ~PerformancePaintTiming() override;

    static GC::Ref<PerformancePaintTiming> create(Utf16String const& paint_type, PaintTimingInfo);

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size() { return 2; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual Utf16FlyString const& entry_type() const override;

private:
    PerformancePaintTiming(Utf16String const& paint_type, PaintTimingInfo);
};

}
