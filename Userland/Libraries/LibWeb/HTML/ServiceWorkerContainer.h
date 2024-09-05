/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

#define ENUMERATE_SERVICE_WORKER_CONTAINER_EVENT_HANDLERS(E)  \
    E(oncontrollerchange, HTML::EventNames::controllerchange) \
    E(onmessage, HTML::EventNames::message)                   \
    E(onmessageerror, HTML::EventNames::messageerror)

namespace Web::HTML {

class ServiceWorkerContainer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ServiceWorkerContainer, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(ServiceWorkerContainer);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ServiceWorkerContainer> create(JS::Realm& realm);
    virtual ~ServiceWorkerContainer() override;

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SERVICE_WORKER_CONTAINER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    explicit ServiceWorkerContainer(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<EnvironmentSettingsObject> m_service_worker_client;
};

}
