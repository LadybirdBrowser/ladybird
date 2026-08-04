/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/MediaQueryListEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CSS {

using MediaQueryListEventInit = Bindings::MediaQueryListEventInit;

class MediaQueryListEvent final : public DOM::Event {
    WEB_WRAPPABLE(MediaQueryListEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MediaQueryListEvent);

public:
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> create(Utf16FlyString const& event_name, Bindings::MediaQueryListEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> create(Utf16FlyString const& event_name, Utf16String media, bool matches, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<MediaQueryListEvent> create_for_constructor(Utf16FlyString const& event_name, Bindings::MediaQueryListEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~MediaQueryListEvent() override;

    Utf16String const& media() const { return m_media; }
    bool matches() const { return m_matches; }

private:
    MediaQueryListEvent(Utf16FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    MediaQueryListEvent(Utf16FlyString const& event_name, Utf16String media, bool matches, HighResolutionTime::DOMHighResTimeStamp);

    Utf16String m_media;
    bool m_matches;
};

}
