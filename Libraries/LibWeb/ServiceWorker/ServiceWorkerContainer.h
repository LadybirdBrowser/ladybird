/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ServiceWorkerContainer.h>
#include <LibWeb/Bindings/ServiceWorkerRegistration.h>
#include <LibWeb/Bindings/Worker.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

#define ENUMERATE_SERVICE_WORKER_CONTAINER_EVENT_HANDLERS(E)  \
    E(oncontrollerchange, HTML::EventNames::controllerchange) \
    E(onmessage, HTML::EventNames::message)                   \
    E(onmessageerror, HTML::EventNames::messageerror)

namespace Web::ServiceWorker {

using RegistrationOptions = Bindings::RegistrationOptions;

class ServiceWorkerContainer : public DOM::EventTarget {
    WEB_WRAPPABLE(ServiceWorkerContainer, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(ServiceWorkerContainer);

public:
    [[nodiscard]] static GC::Ref<ServiceWorkerContainer> create(HTML::EnvironmentSettingsObject&);
    virtual ~ServiceWorkerContainer() override;

    GC::Ref<WebIDL::Promise> ready(JS::Realm&);
    void register_(JS::Realm&, TrustedTypes::TrustedScriptURLOrString script_url, RegistrationOptions const&, GC::Ref<WebIDL::Promise>);
    void get_registration(JS::Realm&, String const& client_url, GC::Ref<WebIDL::Promise>);
    void get_registrations(JS::Realm&, GC::Ref<WebIDL::Promise>);

    void start_ready_promise_steps(GC::Ref<WebIDL::Promise>);
    HTML::EnvironmentSettingsObject& service_worker_client() { return m_service_worker_client; }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SERVICE_WORKER_CONTAINER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    explicit ServiceWorkerContainer(HTML::EnvironmentSettingsObject&);

    virtual void visit_edges(Cell::Visitor&) override;

    void start_register(JS::Realm&, Optional<URL::URL> scope_url, Optional<URL::URL> script_url,
        GC::Ref<WebIDL::Promise>, HTML::EnvironmentSettingsObject&, URL::URL referrer,
        WorkerType, ServiceWorkerUpdateViaCache);

    GC::Ref<HTML::EnvironmentSettingsObject> m_service_worker_client;
};

}
