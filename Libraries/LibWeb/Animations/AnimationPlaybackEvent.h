/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
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

    NullableCSSNumberish current_time() const;
    NullableCSSNumberish timeline_time() const;

private:
    AnimationPlaybackEvent(JS::Realm&, FlyString const& type, AnimationPlaybackEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    using CSSNumberishInternal = Variant<Empty, double, GC::Ref<CSS::CSSNumericValue>>;
    static CSSNumberishInternal to_numberish_internal(Optional<CSS::CSSNumberish> const&);
    static NullableCSSNumberish to_nullable_numberish(CSSNumberishInternal const&);

    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-currenttime
    CSSNumberishInternal m_current_time;

    // https://drafts.csswg.org/web-animations-2/#dom-animationplaybackevent-timelinetime
    CSSNumberishInternal m_timeline_time;
};

}
