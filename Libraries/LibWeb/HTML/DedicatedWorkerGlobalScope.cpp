/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DedicatedWorkerExposedInterfaces.h>
#include <LibWeb/Bindings/DedicatedWorkerGlobalScopePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DedicatedWorkerGlobalScope);

DedicatedWorkerGlobalScope::DedicatedWorkerGlobalScope(JS::Realm& realm, GC::Ref<Web::Page> page)
    : WorkerGlobalScope(realm, page)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .has_global_interface_extended_attribute = true };
}

DedicatedWorkerGlobalScope::~DedicatedWorkerGlobalScope() = default;

void DedicatedWorkerGlobalScope::initialize_web_interfaces_impl()
{
    auto& realm = this->realm();
    add_dedicated_worker_exposed_interfaces(*this);

    DedicatedWorkerGlobalScopeGlobalMixin::initialize(realm, *this);

    Base::initialize_web_interfaces_impl();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-dedicatedworkerglobalscope-close
void DedicatedWorkerGlobalScope::close()
{
    // The close() method steps are to close a worker given this.
    close_a_worker();
}

void DedicatedWorkerGlobalScope::finalize()
{
    Base::finalize();
    WindowOrWorkerGlobalScopeMixin::finalize();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-dedicatedworkerglobalscope-postmessage-options
WebIDL::ExceptionOr<void> DedicatedWorkerGlobalScope::post_message(JS::Value message, StructuredSerializeOptions const& options)
{
    // The postMessage(message, transfer) and postMessage(message, options) methods on DedicatedWorkerGlobalScope objects act as if,
    // when invoked, it immediately invoked the respective postMessage(message, transfer) and postMessage(message, options)
    // on the port, with the same arguments, and returned the same return value.
    return m_internal_port->post_message(message, options);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-dedicatedworkerglobalscope-postmessage
WebIDL::ExceptionOr<void> DedicatedWorkerGlobalScope::post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer)
{
    // The postMessage(message, transfer) and postMessage(message, options) methods on DedicatedWorkerGlobalScope objects act as if,
    // when invoked, it immediately invoked the respective postMessage(message, transfer) and postMessage(message, options)
    // on the port, with the same arguments, and returned the same return value.
    return m_internal_port->post_message(message, transfer);
}

WebIDL::CallbackType* DedicatedWorkerGlobalScope::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

void DedicatedWorkerGlobalScope::set_onmessage(WebIDL::CallbackType* callback)
{
    set_event_handler_attribute(EventNames::message, callback);

    // NOTE: This onmessage attribute setter implicitly sets worker's underlying MessagePort's onmessage attribute, so this
    //       spec behavior also applies here:
    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:handler-messageeventtarget-onmessage
    // The first time a MessagePort object's onmessage IDL attribute is set, the port's port message queue must be enabled,
    // as if the start() method had been called.
    m_internal_port->start();
}

void DedicatedWorkerGlobalScope::set_onmessageerror(WebIDL::CallbackType* callback)
{
    set_event_handler_attribute(EventNames::messageerror, callback);
}

WebIDL::CallbackType* DedicatedWorkerGlobalScope::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

}
