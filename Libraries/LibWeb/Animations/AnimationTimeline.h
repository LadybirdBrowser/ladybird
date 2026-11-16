/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/AnimationTimeline.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/DOM/AbstractElement.h>

namespace Web::Animations {

// https://drafts.csswg.org/scroll-animations-1/#enumdef-scrollaxis
enum class ScrollAxis : u8 {
    Block,
    Inline,
    X,
    Y,
};

ScrollAxis scroll_axis_from_css_axis(CSS::Axis);

// One side of the view progress visibility range: auto takes the scroll container's scroll padding, otherwise a length
// plus a percentage of the scrollport.
// https://drafts.csswg.org/scroll-animations-1/#view-timeline-inset
struct ViewTimelineInsetEdge {
    bool is_auto { true };
    double px { 0 };
    double percentage { 0 };
    // NB: An inset only a calculation can describe, such as min(50%, 10px), resolves against the scrollport size.
    CSS::RustStyleValueHandle calculated {};

    bool operator==(ViewTimelineInsetEdge const&) const = default;
};

struct ViewTimelineInset {
    ViewTimelineInsetEdge start;
    ViewTimelineInsetEdge end;

    bool operator==(ViewTimelineInset const&) const = default;
};

// https://drafts.csswg.org/web-animations-2/#the-animationtimeline-interface
// The cell of an AnimationTimeline, which Bindings/Rust/src/animations implements, of any of the interfaces that
// inherit from it. The state of the timeline is its record.
class WEB_API AnimationTimeline final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AnimationTimeline, animation_timeline)

public:
    // The source of an anonymous scroll timeline: the scroller it finds from its target.
    // https://drafts.csswg.org/scroll-animations-1/#scroll-notation
    struct AnonymousScrollSource {
        CSS::Scroller scroller;
        DOM::AbstractElement target;

        bool operator==(AnonymousScrollSource const& other) const = default;
    };
    using ScrollSource = Variant<GC::Ptr<DOM::Element const>, AnonymousScrollSource>;

    static GC::Ref<AnimationTimeline> create_document_timeline(DOM::Document&, double origin_time);
    static GC::Ref<AnimationTimeline> create_scroll_timeline(DOM::Document&, ScrollSource const&, ScrollAxis);
    static GC::Ref<AnimationTimeline> create_view_timeline(DOM::Document&, DOM::AbstractElement subject, ScrollAxis, ViewTimelineInset const&);
    static GC::Ref<AnimationTimeline> create_internal_timeline(DOM::Document&);

    class CurrentTimeOverrideScope {
    public:
        CurrentTimeOverrideScope(AnimationTimeline&, Optional<TimeValue>);
        ~CurrentTimeOverrideScope();

        CurrentTimeOverrideScope(CurrentTimeOverrideScope const&) = delete;
        CurrentTimeOverrideScope& operator=(CurrentTimeOverrideScope const&) = delete;

    private:
        GC::Ref<AnimationTimeline> m_timeline;
        bool m_had_previous_current_time_override { false };
        RustFFI::OptionalTimeValue m_previous_current_time_override;
    };

    // The timeline's state, which lives as long as the timeline.
    RustFFI::TimelineCell const& record() const;

    Optional<TimeValue> current_time() const;
    // What the timing model reads of the timeline now.
    RustFFI::TimelineSample sample() const { return read().sample; }
    // What moves the timeline as a clock tick samples it.
    RustFFI::TimelineClock clock() const { return read().clock; }
    Optional<TimeValue> current_time_for_observation();
    // Whether script observed the timeline in the current task at a time ahead of its current time.
    bool was_observed_ahead_in_current_task() const;
    // The current time at a timestamp, where the timeline can sample it there, and its current time otherwise.
    Optional<TimeValue> current_time_at_timestamp(double timestamp) const;

    // Runs the callback with every timeline in [timelines, end) that script observed ahead of its current time in the
    // current task sampled at the time script observed, so that what the callback samples agrees with what script read.
    template<typename Iterator, typename Callback>
    static void at_times_observed_ahead(Iterator timelines, Iterator end, Callback const& callback);

    // Updates the current time: a document timeline's from the timestamp, and a scroll timeline's from the layout of
    // its scroller, which ignores the timestamp.
    void update_current_time(double timestamp);
    // Updates the current time of a scroll timeline from the layout of its scroller where that changed it since its
    // last update, answering whether it did.
    bool update_current_time_if_stale();

    Optional<TimeValue> duration() const;

    GC::Ref<DOM::Document> associated_document() const;

    bool is_inactive() const { return read().is_inactive; }
    bool is_monotonically_increasing() const { return read().sample.is_monotonically_increasing; }
    bool is_document_timeline() const { return read().kind == RustFFI::FfiTimelineKind::Document; }
    // A scroll timeline, or a view timeline, which is one.
    bool is_scroll_timeline() const;
    bool is_view_timeline() const { return read().kind == RustFFI::FfiTimelineKind::View; }
    bool is_progress_based() const { return is_scroll_timeline(); }

    // https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
    Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<TimeValue>);
    bool can_convert_a_timeline_time_to_an_origin_relative_time() const { return is_document_timeline(); }

    // NB: The animations associated with the timeline, none of which it keeps alive.
    WeakAnimationSetView associated_animations() const { return WeakAnimationSetView { *RustFFI::animations_timeline_associated_animations(&record()) }; }

    // The source, axis and inset of a scroll or view timeline.
    ScrollSource scroll_source() const;
    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    GC::Ptr<DOM::Element const> source() const;
    ScrollAxis axis() const { return static_cast<ScrollAxis>(read().axis); }
    void set_axis(ScrollAxis);
    DOM::AbstractElement view_subject() const;
    ViewTimelineInset view_inset() const;
    void set_axis_and_inset(ScrollAxis, ViewTimelineInset const&);

    // Sets the time of the Internals interface's timeline, or the time a read of it observes.
    void set_internal_time(Optional<double>, bool for_observation);

private:
    friend class DOM::Document;

    static GC::Ref<AnimationTimeline> create(DOM::Document&, RustFFI::FfiTimelineDescription const&);

    RustFFI::FfiTimeline read() const { return RustFFI::animations_timeline_read(&record()); }
    u64 task_generation() const;
    void update_associated_animations();
    // The host's function through which the record has the timeline update its associated animations.
    static void update_associated_animations_of(void* timeline);

    // Updates the current time of a scroll timeline from where the render state lays out the boxes the timeline
    // follows, unless that leaves it as it was where `only_if_stale`, answering whether it updated it.
    bool update_current_time_from_layout(bool only_if_stale);

    void set_current_time_override_for_style_sampling(Optional<TimeValue> value)
    {
        RustFFI::animations_timeline_set_style_sampling_override(&record(), true, to_ffi(value));
    }
    void clear_current_time_override_for_style_sampling() { RustFFI::animations_timeline_set_style_sampling_override(&record(), false, to_ffi(Optional<TimeValue> {})); }
};

ViewTimelineInset view_timeline_inset_from_ffi(CSS::ComputedValuesFFI::FfiViewTimelineInset const&);

template<typename Iterator, typename Callback>
void AnimationTimeline::at_times_observed_ahead(Iterator timelines, Iterator end, Callback const& callback)
{
    if (timelines == end)
        return callback();
    auto& timeline = **timelines;
    ++timelines;
    if (!timeline.was_observed_ahead_in_current_task())
        return at_times_observed_ahead(timelines, end, callback);
    CurrentTimeOverrideScope override { timeline, timeline.current_time_for_observation() };
    at_times_observed_ahead(timelines, end, callback);
}

}

// NB: What the timeline records ask of the document a timeline is associated with.
extern "C" WEB_API bool ladybird_animations_document_is_active(void* document);
extern "C" WEB_API void* ladybird_animations_element_node_document(void* element);
