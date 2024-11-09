/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#dictdef-animationplaybackeventinit
struct AnimationPlaybackEventInit : public DOM::EventInit {
    Optional<double> current_time {};
    Optional<double> timeline_time {};
};

// https://www.w3.org/TR/web-animations-1/#animationplaybackevent
class AnimationPlaybackEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(AnimationPlaybackEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(AnimationPlaybackEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<AnimationPlaybackEvent> create(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AnimationPlaybackEvent>> construct_impl(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init);

    virtual ~AnimationPlaybackEvent() override = default;

    Optional<double> current_time() const { return m_current_time; }
    void set_current_time(Optional<double> current_time) { m_current_time = current_time; }

    Optional<double> timeline_time() const { return m_timeline_time; }
    void set_timeline_time(Optional<double> timeline_time) { m_timeline_time = timeline_time; }

private:
    AnimationPlaybackEvent(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    // https://www.w3.org/TR/web-animations-1/#dom-animationplaybackeventinit-currenttime
    Optional<double> m_current_time {};

    // https://www.w3.org/TR/web-animations-1/#dom-animationplaybackeventinit-timelinetime
    Optional<double> m_timeline_time {};
};

}
