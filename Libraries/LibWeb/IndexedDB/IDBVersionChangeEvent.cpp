/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/IDBVersionChangeEvent.h>
#include <LibWeb/IndexedDB/IDBVersionChangeEvent.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBVersionChangeEvent);

GC::Ref<IDBVersionChangeEvent> IDBVersionChangeEvent::create(FlyString const& event_name, IDBVersionChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<IDBVersionChangeEvent>(event_name, event_init, time_stamp);
}

GC::Ref<IDBVersionChangeEvent> IDBVersionChangeEvent::create(FlyString const& event_name, Bindings::IDBVersionChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    IDBVersionChangeEventInit init;
    init.bubbles = event_init.bubbles;
    init.cancelable = event_init.cancelable;
    init.composed = event_init.composed;
    init.old_version = event_init.old_version;
    init.new_version = event_init.new_version.value_or({});
    return create(event_name, init, time_stamp);
}

IDBVersionChangeEvent::IDBVersionChangeEvent(FlyString const& event_name, IDBVersionChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_old_version(event_init.old_version)
    , m_new_version(event_init.new_version)
{
}

IDBVersionChangeEvent::~IDBVersionChangeEvent() = default;

}
