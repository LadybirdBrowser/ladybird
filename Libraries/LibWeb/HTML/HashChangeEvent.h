/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/HashChangeEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

using HashChangeEventInit = Bindings::HashChangeEventInit;

class HashChangeEvent final : public DOM::Event {
    WEB_WRAPPABLE(HashChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(HashChangeEvent);

public:
    [[nodiscard]] static GC::Ref<HashChangeEvent> create(FlyString const& event_name, HashChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<HashChangeEvent> create(Utf16String const& event_name, HashChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<HashChangeEvent> create(FlyString const& event_name, String old_url, String new_url, HighResolutionTime::DOMHighResTimeStamp);

    Utf16String const& old_url() const { return m_old_url; }
    Utf16String const& new_url() const { return m_new_url; }

private:
    HashChangeEvent(FlyString const& event_name, HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    HashChangeEvent(Utf16FlyString const& event_name, HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    HashChangeEvent(FlyString const& event_name, String old_url, String new_url, HighResolutionTime::DOMHighResTimeStamp);

    Utf16String m_old_url;
    Utf16String m_new_url;
};

}
