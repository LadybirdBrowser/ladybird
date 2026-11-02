/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/Bindings/ViewTimeline.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::Animations {

// https://drafts.csswg.org/scroll-animations-1/#viewtimeline
class ViewTimeline final : public ScrollTimeline {
    WEB_WRAPPABLE(ViewTimeline, ScrollTimeline);
    GC_DECLARE_ALLOCATOR(ViewTimeline);

public:
    using InsetEdge = ViewTimelineInsetEdge;
    using Inset = ViewTimelineInset;

    static GC::Ref<ViewTimeline> create(DOM::Document&, DOM::AbstractElement subject, ScrollAxis, Inset);
    static WebIDL::ExceptionOr<GC::Ref<ViewTimeline>> create_for_constructor(JS::Object&, Bindings::ViewTimelineOptions const&);

    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-subject
    GC::Ref<DOM::Element const> subject() const;
    DOM::AbstractElement subject_internal() const;

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    virtual GC::Ptr<DOM::Element const> source_for_bindings() const override;

    Inset const& inset() const { return m_inset; }
    void set_axis_and_inset(ScrollAxis, Inset);

    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-startoffset
    GC::Ref<CSS::CSSNumericValue> start_offset() const;
    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-endoffset
    GC::Ref<CSS::CSSNumericValue> end_offset() const;

    virtual bool is_stale() const override;
    virtual void update_current_time(double timestamp) override;
    virtual AttachmentInterval interval_for_attachment_range(AnimationAttachmentRange const&) const override;
    virtual Optional<TimeValue> minimum_timeline_time() const override;
    virtual Optional<TimeValue> maximum_timeline_time() const override;

    // What decides the progress of the timeline, along its axis, in CSS pixels.
    struct Metrics {
        Optional<FollowedScroller> followed_scroller;
        double scroll_offset { 0 };
        double max_scroll_offset { 0 };
        double scrollport_size { 0 };
        // The insets of the view progress visibility range, at the start and at the end of the scrollport.
        double inset_start { 0 };
        double inset_end { 0 };
        // Where the subject's border box starts, from the start of the scrollport, and its size.
        double subject_offset { 0 };
        double subject_size { 0 };
        // The start and the end of the cover range.
        double cover_start() const { return subject_offset - (scrollport_size - inset_end); }
        double cover_end() const { return subject_offset + subject_size - inset_start; }

        bool operator==(Metrics const&) const = default;
    };
    // What the timeline's progress was last calculated from: empty while it was inactive.
    Optional<Metrics> const& last_metrics() const { return m_last_metrics; }

private:
    // The scroll offsets at the start and at the end of a named timeline range, in CSS pixels.
    struct ScrollOffsetInterval {
        double start { 0 };
        double end { 0 };
    };

    ViewTimeline(DOM::Document&, DOM::AbstractElement subject, ScrollAxis, Inset);
    virtual ~ViewTimeline() override = default;

    Optional<Metrics> compute_metrics() const;

    ScrollOffsetInterval interval_for_range_name(Metrics const&, TimelineRangeName) const;

    Inset m_inset;
    Optional<Metrics> m_last_metrics;
};

ViewTimelineInset view_timeline_inset_from_ffi(CSS::ComputedValuesFFI::FfiViewTimelineInset const&);

}
