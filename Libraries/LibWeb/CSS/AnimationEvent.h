/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/AnimationEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CSS {

using AnimationEventInit = Bindings::AnimationEventInit;

// https://www.w3.org/TR/css-animations-1/#animationevent
class AnimationEvent : public DOM::Event {
    WEB_WRAPPABLE(AnimationEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AnimationEvent);

public:
    [[nodiscard]] static GC::Ref<AnimationEvent> create(Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<AnimationEvent> create(Utf16FlyString const& type, Utf16String animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<AnimationEvent>> create_for_constructor(Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~AnimationEvent() override = default;

    Utf16String const& animation_name() const { return m_animation_name; }
    double elapsed_time() const { return m_elapsed_time; }
    Utf16String const& pseudo_element() const { return m_pseudo_element; }
    GC::Ptr<CSSAnimation> animation() const { return m_animation; }
    static constexpr size_t animation_offset() { return offsetof(AnimationEvent, m_animation); }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    AnimationEvent(Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    AnimationEvent(Utf16FlyString const& type, Utf16String animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp);

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-animationname
    Utf16String m_animation_name {};

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-elapsedtime
    double m_elapsed_time { 0.0 };

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-pseudoelement
    Utf16String m_pseudo_element {};

    // https://drafts.csswg.org/css-animations-2/#dom-animationevent-animation
    GC::Ptr<CSSAnimation> m_animation;
};

}
