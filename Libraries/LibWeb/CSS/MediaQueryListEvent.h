/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MediaQueryListEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CSS {

class MediaQueryListEvent;

}

namespace Web::Bindings {

class WrapperWorld;
WEB_API JS::Realm& wrapper_realm_for_media_query_list_event(WrapperWorld const&, JS::Realm&, CSS::MediaQueryListEvent&);

}

namespace Web::CSS {

using MediaQueryListEventInit = Bindings::MediaQueryListEventInit;

class MediaQueryListEvent final : public DOM::Event {
    WEB_WRAPPABLE(MediaQueryListEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MediaQueryListEvent);

public:
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> create(
        FlyString const& event_name, MediaQueryListEventInit const&,
        HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> create(
        FlyString const& event_name, String media, bool matches,
        HighResolutionTime::DOMHighResTimeStamp);

    virtual ~MediaQueryListEvent() override;

    String const& media() const { return m_media; }
    bool matches() const { return m_matches; }

private:
    MediaQueryListEvent(FlyString const& event_name, MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    MediaQueryListEvent(FlyString const& event_name, String media, bool matches, HighResolutionTime::DOMHighResTimeStamp);

    String m_media;
    bool m_matches;
};

}
