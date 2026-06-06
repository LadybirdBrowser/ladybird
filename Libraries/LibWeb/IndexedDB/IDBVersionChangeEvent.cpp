/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/IDBVersionChangeEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/IndexedDB/IDBVersionChangeEvent.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBVersionChangeEvent);

GC::Ref<IDBVersionChangeEvent> IDBVersionChangeEvent::create(FlyString const& event_name, Bindings::IDBVersionChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<IDBVersionChangeEvent>(event_name, event_init, time_stamp);
}

GC::Ref<IDBVersionChangeEvent> IDBVersionChangeEvent::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, FlyString const& event_name, Bindings::IDBVersionChangeEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

IDBVersionChangeEvent::IDBVersionChangeEvent(FlyString const& event_name, Bindings::IDBVersionChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_old_version(event_init.old_version)
    , m_new_version(event_init.new_version.value_or({}))
{
}

IDBVersionChangeEvent::~IDBVersionChangeEvent() = default;

}
