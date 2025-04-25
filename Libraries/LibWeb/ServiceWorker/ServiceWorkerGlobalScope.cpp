/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ServiceWorker/EventNames.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerGlobalScope);

ServiceWorkerGlobalScope::~ServiceWorkerGlobalScope() = default;

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(JS::Realm& realm, GC::Ref<Web::Page> page)
    : HTML::WorkerGlobalScope(realm, page)
{
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
void ServiceWorkerGlobalScope::set_oninstall(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::install, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::oninstall()
{
    return event_handler_attribute(EventNames::install);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
void ServiceWorkerGlobalScope::set_onactivate(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::activate, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onactivate()
{
    return event_handler_attribute(EventNames::activate);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
void ServiceWorkerGlobalScope::set_onfetch(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::fetch, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onfetch()
{
    return event_handler_attribute(EventNames::fetch);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
void ServiceWorkerGlobalScope::set_onmessage(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::message, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
void ServiceWorkerGlobalScope::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::messageerror, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

}
