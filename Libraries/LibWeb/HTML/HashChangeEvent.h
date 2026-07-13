/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/HashChangeEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

class HashChangeEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(HashChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(HashChangeEvent);

public:
    [[nodiscard]] static GC::Ref<HashChangeEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::HashChangeEventInit const&);
    [[nodiscard]] static GC::Ref<HashChangeEvent> construct_impl(JS::Realm&, Utf16FlyString const& event_name, Bindings::HashChangeEventInit const&);

    Utf16String const& old_url() const { return m_old_url; }
    Utf16String const& new_url() const { return m_new_url; }

private:
    HashChangeEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::HashChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    Utf16String m_old_url;
    Utf16String m_new_url;
};

}
