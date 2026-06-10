/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorker);

ServiceWorker::ServiceWorker(ServiceWorkerRecord* service_worker_record)
    : DOM::EventTarget()
    , m_service_worker_record(service_worker_record)
{
}

ServiceWorker::~ServiceWorker() = default;

GC::Ref<ServiceWorker> ServiceWorker::create(ServiceWorkerRecord* service_worker_record)
{
    return GC::Heap::the().allocate<ServiceWorker>(service_worker_record);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworker-scripturl
String ServiceWorker::script_url() const
{
    if (!m_service_worker_record)
        return {};

    return m_service_worker_record->script_url.serialize();
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                           \
    void ServiceWorker::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                     \
        set_event_handler_attribute(event_name, value);                   \
    }                                                                     \
    WebIDL::CallbackType* ServiceWorker::attribute_name()                 \
    {                                                                     \
        return event_handler_attribute(event_name);                       \
    }
ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
