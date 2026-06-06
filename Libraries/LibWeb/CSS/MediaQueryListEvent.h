/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MediaQueryListEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CSS {

class MediaQueryListEvent final : public DOM::Event {
    WEB_WRAPPABLE(MediaQueryListEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MediaQueryListEvent);

public:
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> create(
        FlyString const& event_name, Bindings::MediaQueryListEventInit const&,
        HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> construct_impl(HTML::Window&, FlyString const& event_name, Bindings::MediaQueryListEventInit const& = {});

    virtual ~MediaQueryListEvent() override;

    String const& media() const { return m_media; }
    bool matches() const { return m_matches; }

private:
    MediaQueryListEvent(FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    String m_media;
    bool m_matches;
};

}
