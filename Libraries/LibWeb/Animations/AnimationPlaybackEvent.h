/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#dictdef-animationplaybackeventinit
struct AnimationPlaybackEventInit : public DOM::EventInit {
    Optional<CSS::CSSNumberish> current_time;
    Optional<CSS::CSSNumberish> timeline_time;
};

// https://www.w3.org/TR/web-animations-1/#animationplaybackevent
class AnimationPlaybackEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(AnimationPlaybackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AnimationPlaybackEvent);

public:
    [[nodiscard]] static GC::Ref<AnimationPlaybackEvent> create(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init = {});
    static WebIDL::ExceptionOr<GC::Ref<AnimationPlaybackEvent>> construct_impl(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init);

    virtual ~AnimationPlaybackEvent() override = default;

    NullableCSSNumberish current_time() const
    {
        return m_current_time.map([](auto const& value) { return NullableCSSNumberish { value }; }).value_or(Empty {});
    }

    NullableCSSNumberish timeline_time() const
    {
        return m_timeline_time.map([](auto const& value) { return NullableCSSNumberish { value }; }).value_or(Empty {});
    }

private:
    AnimationPlaybackEvent(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-currenttime
    Optional<CSS::CSSNumberish> m_current_time;

    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-timelinetime
    Optional<CSS::CSSNumberish> m_timeline_time;
};

}
