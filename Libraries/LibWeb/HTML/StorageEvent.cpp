/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/StorageEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(StorageEvent);

GC::Ref<StorageEvent> StorageEvent::create(FlyString const& event_name, StorageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<StorageEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

StorageEvent::~StorageEvent() = default;

// https://html.spec.whatwg.org/multipage/webstorage.html#dom-storageevent-initstorageevent
void StorageEvent::init_storage_event(
    String const& type,
    bool bubbles,
    bool cancelable,
    Optional<String> const& key,
    Optional<String> const& old_value,
    Optional<String> const& new_value,
    String const& url,
    GC::Ptr<Storage> storage_area)
{
    // The initStorageEvent(type, bubbles, cancelable, key, oldValue, newValue, url, storageArea) method must initialize
    // the event in a manner analogous to the similarly-named initEvent() method. [DOM]
    if (dispatched())
        return;

    initialize_event(type, bubbles, cancelable);
    m_key = key;
    m_old_value = old_value;
    m_new_value = new_value;
    m_url = url;
    m_storage_area = storage_area;
}

StorageEvent::StorageEvent(FlyString const& event_name, StorageEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_key(event_init.key)
    , m_old_value(event_init.old_value)
    , m_new_value(event_init.new_value)
    , m_url(event_init.url)
    , m_storage_area(event_init.storage_area)
{
}

void StorageEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_storage_area);
}

}
