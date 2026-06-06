/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AnimationEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CSS {

// https://www.w3.org/TR/css-animations-1/#animationevent
class AnimationEvent : public DOM::Event {
    WEB_WRAPPABLE(AnimationEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AnimationEvent);

public:
    [[nodiscard]] static GC::Ref<AnimationEvent> create(FlyString const& type, Bindings::AnimationEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<AnimationEvent>> construct_impl(HTML::Window&, FlyString const& type, Bindings::AnimationEventInit const& event_init);

    virtual ~AnimationEvent() override = default;

    FlyString const& animation_name() const { return m_animation_name; }
    double elapsed_time() const { return m_elapsed_time; }
    FlyString const& pseudo_element() const { return m_pseudo_element; }

private:
    AnimationEvent(FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-animationname
    FlyString m_animation_name {};

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-elapsedtime
    double m_elapsed_time { 0.0 };

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-pseudoelement
    FlyString m_pseudo_element {};
};

}
