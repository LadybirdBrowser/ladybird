/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/WeakHashSet.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#animationtimeline
class AnimationTimeline : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(AnimationTimeline, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(AnimationTimeline);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    class CurrentTimeOverrideScope {
    public:
        CurrentTimeOverrideScope(AnimationTimeline&, Optional<TimeValue>);
        ~CurrentTimeOverrideScope();

        CurrentTimeOverrideScope(CurrentTimeOverrideScope const&) = delete;
        CurrentTimeOverrideScope& operator=(CurrentTimeOverrideScope const&) = delete;

    private:
        GC::Ref<AnimationTimeline> m_timeline;
        Optional<TimeValue> m_previous_current_time_override;
        bool m_had_previous_current_time_override { false };
    };

    NullableCSSNumberish current_time_for_bindings();
    Optional<TimeValue> current_time() const;
    Optional<TimeValue> current_time_for_observation();

    virtual void update_current_time(double timestamp) = 0;

    NullableCSSNumberish duration_for_bindings() const;
    virtual Optional<TimeValue> duration() const = 0;

    GC::Ref<DOM::Document> associated_document() const { return m_associated_document; }

    virtual bool is_inactive() const;
    bool is_monotonically_increasing() const { return m_is_monotonically_increasing; }
    virtual bool is_progress_based() const { return false; }

    // https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
    virtual Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<TimeValue>) { VERIFY_NOT_REACHED(); }
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const { return false; }

    void associate_with_animation(GC::Ref<Animation> value) { m_associated_animations.set(*value); }
    void disassociate_with_animation(GC::Ref<Animation> value) { m_associated_animations.remove(*value); }
    GC::WeakHashSet<Animation> const& associated_animations() const { return m_associated_animations; }

protected:
    explicit AnimationTimeline(GC::Ref<DOM::Document>);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    void set_current_time(Optional<TimeValue> value);
    void update_associated_animations();
    virtual bool can_sample_current_time_at_timestamp() const { return false; }
    virtual Optional<TimeValue> current_time_at_timestamp(double) const { return current_time(); }

    // https://www.w3.org/TR/web-animations-1/#dom-animationtimeline-currenttime
    Optional<TimeValue> m_current_time {};

    // https://drafts.csswg.org/web-animations-1/#monotonically-increasing-timeline
    bool m_is_monotonically_increasing { false };

    // https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    GC::Ref<DOM::Document> m_associated_document;

    GC::WeakHashSet<Animation> m_associated_animations;
    Optional<u64> m_last_current_time_update_task_generation;
    Optional<TimeValue> m_observed_current_time;

private:
    friend class DOM::Document;

    void set_current_time_override_for_style_sampling(Optional<TimeValue> value)
    {
        m_current_time_override_for_style_sampling = move(value);
        m_has_current_time_override_for_style_sampling = true;
    }
    void clear_current_time_override_for_style_sampling() { m_has_current_time_override_for_style_sampling = false; }

    Optional<TimeValue> effective_current_time() const;

    Optional<TimeValue> m_current_time_override_for_style_sampling;
    bool m_has_current_time_override_for_style_sampling { false };
};

}
