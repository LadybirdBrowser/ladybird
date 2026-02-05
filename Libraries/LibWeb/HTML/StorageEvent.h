/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webstorage.html#storageeventinit
struct StorageEventInit : public DOM::EventInit {
    Optional<String> key;
    Optional<String> old_value;
    Optional<String> new_value;
    String url;
    GC::Ptr<Storage> storage_area;
};

// https://html.spec.whatwg.org/multipage/webstorage.html#storageevent
class StorageEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(StorageEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(StorageEvent);

public:
    [[nodiscard]] static GC::Ref<StorageEvent> create(JS::Realm&, FlyString const& event_name, StorageEventInit const& event_init = {});
    static GC::Ref<StorageEvent> construct_impl(JS::Realm&, FlyString const& event_name, StorageEventInit const& event_init);

    virtual ~StorageEvent() override;

    Optional<String> const& key() const { return m_key; }
    Optional<String> const& old_value() const { return m_old_value; }
    Optional<String> const& new_value() const { return m_new_value; }
    String const& url() const { return m_url; }
    GC::Ptr<Storage const> storage_area() const { return m_storage_area; }

    void init_storage_event(String const& type, bool bubbles = false, bool cancelable = false,
        Optional<String> const& key = {}, Optional<String> const& old_value = {}, Optional<String> const& new_value = {},
        String const& url = {}, GC::Ptr<Storage> storage_area = {});

protected:
    virtual void visit_edges(Visitor& visitor) override;
    virtual void initialize(JS::Realm&) override;

private:
    StorageEvent(JS::Realm&, FlyString const& event_name, StorageEventInit const& event_init);

    Optional<String> m_key;
    Optional<String> m_old_value;
    Optional<String> m_new_value;
    String m_url;
    GC::Ptr<Storage> m_storage_area;
};

}
