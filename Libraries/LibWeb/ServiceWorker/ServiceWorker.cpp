/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>

namespace Web::ServiceWorker {

ServiceWorker::ServiceWorker(JS::Realm& realm, String script_url)
    : DOM::EventTarget(realm)
    , m_script_url(move(script_url))
{
}

ServiceWorker::~ServiceWorker() = default;

GC::Ref<ServiceWorker> ServiceWorker::create(JS::Realm& realm)
{
    return realm.create<ServiceWorker>(realm, ""_string);
}

void ServiceWorker::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ServiceWorker);
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
