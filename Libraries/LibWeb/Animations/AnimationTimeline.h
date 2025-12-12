/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#animationtimeline
class AnimationTimeline : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AnimationTimeline, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AnimationTimeline);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    NullableCSSNumberish current_time_for_bindings() const
    {
        return NullableCSSNumberish::from_optional_css_numberish_time(realm(), current_time());
    }
    Optional<TimeValue> current_time() const;

    virtual void update_current_time(double timestamp) = 0;

    NullableCSSNumberish duration_for_bindings() const;
    virtual Optional<TimeValue> duration() const = 0;

    GC::Ptr<DOM::Document> associated_document() const { return m_associated_document; }
    void set_associated_document(GC::Ptr<DOM::Document>);

    virtual bool is_inactive() const;
    bool is_monotonically_increasing() const { return m_is_monotonically_increasing; }
    virtual bool is_progress_based() const { return false; }

    // https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
    virtual Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<TimeValue>) { VERIFY_NOT_REACHED(); }
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const { return false; }

    void associate_with_animation(GC::Ref<Animation> value) { m_associated_animations.set(value); }
    void disassociate_with_animation(GC::Ref<Animation> value) { m_associated_animations.remove(value); }
    HashTable<GC::Weak<Animation>> const& associated_animations() const { return m_associated_animations; }

protected:
    AnimationTimeline(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    void set_current_time(Optional<TimeValue> value);

    // https://www.w3.org/TR/web-animations-1/#dom-animationtimeline-currenttime
    Optional<TimeValue> m_current_time {};

    // https://drafts.csswg.org/web-animations-1/#monotonically-increasing-timeline
    bool m_is_monotonically_increasing { false };

    // https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    GC::Ptr<DOM::Document> m_associated_document {};

    HashTable<GC::Weak<Animation>> m_associated_animations {};
};

}
