/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/LargestContentfulPaint.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PaintTiming/DocumentPaintTimingState.h>
#include <LibWeb/PaintTiming/PaintTimingMixin.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::LargestContentfulPaint {

// https://w3c.github.io/largest-contentful-paint/#sec-largest-contentful-paint-interface
class LargestContentfulPaint final : public PerformanceTimeline::PerformanceEntry {
    WEB_RUST_INTERFACE(LargestContentfulPaint, largest_contentful_paint)

public:
    static GC::Ref<LargestContentfulPaint> create(PaintTiming::PaintTimingInfo, WebIDL::UnsignedLong size, Utf16String url, Utf16String id, HighResolutionTime::DOMHighResTimeStamp load_time, GC::Ref<DOM::Element>);
};

// https://w3c.github.io/largest-contentful-paint/#report-largest-contentful-paint
void report_largest_contentful_paint(DOM::Document&, PaintTiming::PaintTimingInfo const&, Vector<PaintTiming::PendingImageRecord> const& painted_images, PaintTiming::PaintedTextNodes const& painted_text_nodes);

}
