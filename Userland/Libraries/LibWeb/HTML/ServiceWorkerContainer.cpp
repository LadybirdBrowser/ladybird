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

// https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-onmessage
WebIDL::CallbackType* ServiceWorkerContainer::onmessage()
{
    return event_handler_attribute(HTML::EventNames::message);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-onmessage
void ServiceWorkerContainer::set_onmessage(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::message, event_handler);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-onmessageerror
WebIDL::CallbackType* ServiceWorkerContainer::onmessageerror()
{
    return event_handler_attribute(HTML::EventNames::messageerror);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkercontainer-onmessageerror
void ServiceWorkerContainer::set_onmessageerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::messageerror, event_handler);
}

}
