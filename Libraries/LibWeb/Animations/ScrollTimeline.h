/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/ScrollTimeline.h>

namespace Web::HTML {

class Window;

}

namespace Web::Animations {

using ScrollAxis = Bindings::ScrollAxis;

// https://drafts.csswg.org/scroll-animations-1/#scrolltimeline
class ScrollTimeline : public AnimationTimeline {
    WEB_WRAPPABLE(ScrollTimeline, AnimationTimeline);
    GC_DECLARE_ALLOCATOR(ScrollTimeline);

public:
    struct AnonymousSource {
        CSS::Scroller scroller;
        DOM::AbstractElement target;

        bool operator==(AnonymousSource const& other) const = default;
    };

    using Source = Variant<GC::Ptr<DOM::Element const>, AnonymousSource>;

    static GC::Ref<ScrollTimeline> create(DOM::Document&, Source source, ScrollAxis axis);
    static GC::Ref<ScrollTimeline> create_for_constructor(JS::Realm&, Bindings::ScrollTimelineOptions const&);

    virtual Optional<TimeValue> duration() const override { return TimeValue { TimeValue::Type::Percentage, 100 }; }

    GC::Ptr<DOM::Element const> source() const;
    ScrollAxis axis() const { return m_axis; }

    Source source_internal() const { return m_source; }

    bool is_stale() const;
    virtual void update_current_time(double timestamp) override;

    virtual bool is_progress_based() const override { return true; }
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const override { return false; }

private:
    ScrollTimeline(DOM::Document&, Source source, ScrollAxis axis);
    virtual ~ScrollTimeline() override = default;

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Variant<GC::Ptr<DOM::Element const>, GC::Ptr<DOM::Document>> get_propagated_source() const;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    Source m_source;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-axis
    ScrollAxis m_axis;

    Optional<double> m_last_max_scroll_offset;
};

ScrollAxis scroll_axis_from_css_axis(CSS::Axis);

}
