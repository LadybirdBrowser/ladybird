/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
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
    [[nodiscard]] static GC::Ref<AnimationEvent> create(FlyString const& type, AnimationEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<AnimationEvent> create(FlyString const& type, FlyString animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~AnimationEvent() override = default;

    FlyString const& animation_name() const { return m_animation_name; }
    double elapsed_time() const { return m_elapsed_time; }
    FlyString const& pseudo_element() const { return m_pseudo_element; }

private:
    AnimationEvent(FlyString const& type, AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    AnimationEvent(FlyString const& type, FlyString animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp);

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-animationname
    FlyString m_animation_name {};

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-elapsedtime
    double m_elapsed_time { 0.0 };

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-pseudoelement
    FlyString m_pseudo_element {};
};

}
