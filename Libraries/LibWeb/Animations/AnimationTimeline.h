/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#animationtimeline
class AnimationTimeline : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AnimationTimeline, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(AnimationTimeline);

public:
    Optional<double> current_time() const { return m_current_time; }
    virtual void set_current_time(Optional<double>);

    JS::GCPtr<DOM::Document> associated_document() const { return m_associated_document; }
    void set_associated_document(JS::GCPtr<DOM::Document>);

    virtual bool is_inactive() const;
    bool is_monotonically_increasing() const { return m_is_monotonically_increasing; }

    // https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
    virtual Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<double>) { VERIFY_NOT_REACHED(); }
    virtual bool can_convert_a_timeline_time_to_an_origin_relative_time() const { return false; }

    void associate_with_animation(JS::NonnullGCPtr<Animation> value) { m_associated_animations.set(value); }
    void disassociate_with_animation(JS::NonnullGCPtr<Animation> value) { m_associated_animations.remove(value); }
    HashTable<JS::NonnullGCPtr<Animation>> const& associated_animations() const { return m_associated_animations; }

protected:
    AnimationTimeline(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // https://www.w3.org/TR/web-animations-1/#dom-animationtimeline-currenttime
    Optional<double> m_current_time {};

    // https://www.w3.org/TR/web-animations-1/#monotonically-increasing-timeline
    bool m_is_monotonically_increasing { true };

    // https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    JS::GCPtr<DOM::Document> m_associated_document {};

    HashTable<JS::NonnullGCPtr<Animation>> m_associated_animations {};
};

}
