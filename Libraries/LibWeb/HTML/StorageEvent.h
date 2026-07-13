/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/StorageEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webstorage.html#storageevent
class StorageEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(StorageEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(StorageEvent);

public:
    [[nodiscard]] static GC::Ref<StorageEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::StorageEventInit const& event_init = {});
    static GC::Ref<StorageEvent> construct_impl(JS::Realm&, Utf16FlyString const& event_name, Bindings::StorageEventInit const& event_init);

    virtual ~StorageEvent() override;

    Optional<Utf16String> const& key() const { return m_key; }
    Optional<Utf16String> const& old_value() const { return m_old_value; }
    Optional<Utf16String> const& new_value() const { return m_new_value; }
    Utf16String const& url() const { return m_url; }
    GC::Ptr<Storage const> storage_area() const { return m_storage_area; }

    void init_storage_event(Utf16FlyString const& type,
        bool bubbles = false,
        bool cancelable = false,
        Optional<Utf16String> const& key = {},
        Optional<Utf16String> const& old_value = {},
        Optional<Utf16String> const& new_value = {},
        Utf16View url = {},
        GC::Ptr<Storage> storage_area = {});

protected:
    virtual void visit_edges(Visitor& visitor) override;
    virtual void initialize(JS::Realm&) override;

private:
    StorageEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::StorageEventInit const& event_init);

    Optional<Utf16String> m_key;
    Optional<Utf16String> m_old_value;
    Optional<Utf16String> m_new_value;
    Utf16String m_url;
    GC::Ptr<Storage> m_storage_area;
};

}
