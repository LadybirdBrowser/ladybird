/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/AnimationPlaybackEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::Animations {

// https://www.w3.org/TR/web-animations-1/#animationplaybackevent
class AnimationPlaybackEvent : public DOM::Event {
    WEB_WRAPPABLE(AnimationPlaybackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AnimationPlaybackEvent);

public:
    [[nodiscard]] static GC::Ref<AnimationPlaybackEvent> create(FlyString const& type, Bindings::AnimationPlaybackEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<AnimationPlaybackEvent>> construct_impl(HTML::Window&, FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init);

    virtual ~AnimationPlaybackEvent() override = default;

    NullableCSSNumberish current_time() const;
    NullableCSSNumberish timeline_time() const;

private:
    AnimationPlaybackEvent(FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Visitor&) override;

    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-currenttime
    NullableCSSNumberish m_current_time;

    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-timelinetime
    NullableCSSNumberish m_timeline_time;
};

}
