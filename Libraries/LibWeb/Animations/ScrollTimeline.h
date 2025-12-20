/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ScrollTimelinePrototype.h>

namespace Web::Animations {

// https://drafts.csswg.org/scroll-animations-1/#dictdef-scrolltimelineoptions
struct ScrollTimelineOptions {
    // NB: We use Optional here to distinguish between "undefined" and "null"
    Optional<GC::Ptr<DOM::Element>> source;
    Bindings::ScrollAxis axis;
};

// https://drafts.csswg.org/scroll-animations-1/#scrolltimeline
class ScrollTimeline : public AnimationTimeline {
    WEB_PLATFORM_OBJECT(ScrollTimeline, AnimationTimeline);
    GC_DECLARE_ALLOCATOR(ScrollTimeline);

public:
    static GC::Ref<ScrollTimeline> create(JS::Realm&, DOM::Document&, GC::Ptr<DOM::Element const> source, Bindings::ScrollAxis axis);
    static GC::Ref<ScrollTimeline> construct_impl(JS::Realm&, ScrollTimelineOptions options = {});

    virtual Optional<TimeValue> duration() const override { return TimeValue { TimeValue::Type::Percentage, 100 }; }

    GC::Ptr<DOM::Element const> source() const { return m_source; }
    Bindings::ScrollAxis axis() const { return m_axis; }

    virtual void update_current_time(double timestamp) override;

    virtual bool is_progress_based() const override { return true; }
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const override { return false; }

private:
    ScrollTimeline(JS::Realm&, DOM::Document&, GC::Ptr<DOM::Element const> source, Bindings::ScrollAxis axis);
    virtual ~ScrollTimeline() override = default;

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void initialize(JS::Realm&) override;

    Variant<GC::Ptr<DOM::Element const>, GC::Ptr<DOM::Document>> get_propagated_source() const;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    GC::Ptr<DOM::Element const> m_source;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-axis
    Bindings::ScrollAxis m_axis { Bindings::ScrollAxis::Block };
};

}
