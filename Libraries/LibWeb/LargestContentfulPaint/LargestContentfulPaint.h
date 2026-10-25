/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/PaintTiming/DocumentPaintTimingState.h>
#include <LibWeb/PaintTiming/PaintTimingMixin.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::LargestContentfulPaint {

// https://w3c.github.io/largest-contentful-paint/#sec-largest-contentful-paint-interface
class LargestContentfulPaint final
    : public PerformanceTimeline::PerformanceEntry
    , public PaintTiming::PaintTimingMixin {
    WEB_WRAPPABLE(LargestContentfulPaint, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(LargestContentfulPaint);

public:
    static GC::Ref<LargestContentfulPaint> create(PaintTiming::PaintTimingInfo, WebIDL::UnsignedLong size, Utf16String url, Utf16String id, HighResolutionTime::DOMHighResTimeStamp load_time, GC::Ref<DOM::Element>);

    virtual ~LargestContentfulPaint() override;

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::No; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size() { return 150; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual Utf16FlyString const& entry_type() const override;

    HighResolutionTime::DOMHighResTimeStamp load_time() const { return m_load_time; }
    HighResolutionTime::DOMHighResTimeStamp render_time() const;
    WebIDL::UnsignedLong size() const { return m_size; }
    Utf16String const& id() const { return m_id; }
    Utf16String const& url() const { return m_url; }
    GC::Ptr<DOM::Element> element() const;

private:
    LargestContentfulPaint(PaintTiming::PaintTimingInfo, WebIDL::UnsignedLong size, Utf16String url, Utf16String id, HighResolutionTime::DOMHighResTimeStamp load_time, GC::Ref<DOM::Element>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-size
    WebIDL::UnsignedLong m_size { 0 };

    // https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-loadtime
    HighResolutionTime::DOMHighResTimeStamp m_load_time { 0 };

    // https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-id
    Utf16String m_id;

    // https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-url
    Utf16String m_url;

    // https://w3c.github.io/largest-contentful-paint/#largestcontentfulpaint-element
    GC::Ptr<DOM::Element> m_element;
};

// https://w3c.github.io/largest-contentful-paint/#report-largest-contentful-paint
void report_largest_contentful_paint(DOM::Document&, PaintTiming::PaintTimingInfo const&, Vector<PaintTiming::PendingImageRecord> const& painted_images, PaintTiming::PaintedTextNodes const& painted_text_nodes);

}
