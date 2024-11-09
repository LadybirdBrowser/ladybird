/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-animations-1/#dictdef-animationeventinit
struct AnimationEventInit : public DOM::EventInit {
    FlyString animation_name { ""_fly_string };
    double elapsed_time { 0.0 };
    FlyString pseudo_element { ""_fly_string };
};

// https://www.w3.org/TR/css-animations-1/#animationevent
class AnimationEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(AnimationEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(AnimationEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<AnimationEvent> create(JS::Realm&, FlyString const& type, AnimationEventInit const& event_init = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AnimationEvent>> construct_impl(JS::Realm&, FlyString const& type, AnimationEventInit const& event_init);

    virtual ~AnimationEvent() override = default;

    FlyString const& animation_name() const { return m_animation_name; }
    double elapsed_time() const { return m_elapsed_time; }
    FlyString const& pseudo_element() const { return m_pseudo_element; }

private:
    AnimationEvent(JS::Realm&, FlyString const& type, AnimationEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-animationname
    FlyString m_animation_name {};

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-elapsedtime
    double m_elapsed_time { 0.0 };

    // https://www.w3.org/TR/css-animations-1/#dom-animationevent-pseudoelement
    FlyString m_pseudo_element {};
};

}
