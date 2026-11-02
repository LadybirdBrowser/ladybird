/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibJS/Forward.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/ScrollTimeline.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>

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
    static GC::Ref<ScrollTimeline> create_for_constructor(JS::Object&, Bindings::ScrollTimelineOptions const&);

    virtual Optional<TimeValue> duration() const override { return TimeValue { TimeValue::Type::Percentage, 100 }; }

    GC::Ptr<DOM::Element const> source() const;
    virtual GC::Ptr<DOM::Element const> source_for_bindings() const { return source(); }
    ScrollAxis axis() const { return m_axis; }
    void set_axis(ScrollAxis);

    Source source_internal() const { return m_source; }

    virtual bool is_stale() const;
    virtual void update_current_time(double timestamp) override;

    // The scroll node the timeline followed at its last update, the style node of the element whose box scrolls, or 0
    // for the viewport, and the scroll offset there and at 100% progress, in CSS pixels: empty while it was inactive.
    struct FollowedScroller {
        AsyncScrollNodeStableID scroll_node;
        u32 scroller_style_node { 0 };
        bool vertical { true };
        double scroll_offset { 0 };
        double max_scroll_offset { 0 };

        bool operator==(FollowedScroller const&) const = default;
    };
    Optional<FollowedScroller> const& followed_scroller() const { return m_followed_scroller; }

    // What names the timeline to the render clock, whose ticks sample its animations.
    u64 clock_identity() const { return m_clock_identity; }

    virtual bool is_progress_based() const override { return true; }
    virtual AttachmentInterval interval_for_attachment_range(AnimationAttachmentRange const&) const override;

    // NB: The minimum and the maximum scroll offsets.
    virtual Optional<TimeValue> minimum_timeline_time() const override { return TimeValue { TimeValue::Type::Percentage, 0 }; }
    virtual Optional<TimeValue> maximum_timeline_time() const override { return TimeValue { TimeValue::Type::Percentage, 100 }; }
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const override { return false; }

protected:
    ScrollTimeline(DOM::Document&, Source source, ScrollAxis axis);
    virtual ~ScrollTimeline() override = default;

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // The scroll container the timeline follows, along the timeline's axis, in CSS pixels.
    struct ScrollerMetrics {
        Optional<AsyncScrollNodeStableID> scroll_node;
        u32 scroller_style_node { 0 };
        bool is_vertical { true };
        double scroll_offset { 0 };
        double max_scroll_offset { 0 };
        // Where the scrollport starts in the coordinates the boxes inside it are laid out in, and its size.
        double scrollport_start { 0 };
        double scrollport_size { 0 };
        // The scroll padding at the start and at the end of the scrollport.
        double scroll_padding_start { 0 };
        double scroll_padding_end { 0 };
    };
    // Empty while the source has no box, is not a scroll container or has no scrollable overflow.
    Optional<ScrollerMetrics> scroller_metrics() const;

    static Optional<FollowedScroller> followed_scroller_from(ScrollerMetrics const&);
    void set_followed_scroller(Optional<FollowedScroller> followed_scroller) { m_followed_scroller = move(followed_scroller); }

private:
    Variant<GC::Ptr<DOM::Element const>, GC::Ptr<DOM::Document>> get_propagated_source() const;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    Source m_source;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-axis
    ScrollAxis m_axis;

    Optional<double> m_last_max_scroll_offset;
    Optional<FollowedScroller> m_followed_scroller;
    u64 m_clock_identity { 0 };
};

ScrollAxis scroll_axis_from_css_axis(CSS::Axis);

}
