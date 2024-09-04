/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ServiceWorkerContainerPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/ServiceWorkerContainer.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ServiceWorkerContainer);

ServiceWorkerContainer::ServiceWorkerContainer(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void ServiceWorkerContainer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ServiceWorkerContainer);
}

JS::NonnullGCPtr<ServiceWorkerContainer> ServiceWorkerContainer::create(JS::Realm& realm)
{
    return realm.heap().allocate<ServiceWorkerContainer>(realm, realm);
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                    \
    void ServiceWorkerContainer::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                              \
        set_event_handler_attribute(event_name, move(value));                      \
    }                                                                              \
    WebIDL::CallbackType* ServiceWorkerContainer::attribute_name()                 \
    {                                                                              \
        return event_handler_attribute(event_name);                                \
    }
ENUMERATE_SERVICE_WORKER_CONTAINER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
