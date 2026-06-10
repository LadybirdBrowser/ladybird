/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ServiceWorker.h>
#include <LibWeb/DOM/EventTarget.h>

#define ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(E)  \
    E(onstatechange, HTML::EventNames::statechange) \
    E(onerror, HTML::EventNames::error)

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworker-interface
class ServiceWorker : public DOM::EventTarget {
    WEB_WRAPPABLE(ServiceWorker, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(ServiceWorker);

public:
    [[nodiscard]] static GC::Ref<ServiceWorker> create(ServiceWorkerRecord*);

    virtual ~ServiceWorker() override;

    String script_url() const;
    ServiceWorkerState service_worker_state() const { return m_state; }
    void set_service_worker_state(ServiceWorkerState state) { m_state = state; }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    ServiceWorker(ServiceWorkerRecord*);

    ServiceWorkerState m_state { ServiceWorkerState::Parsed };
    ServiceWorkerRecord* m_service_worker_record;
};

}
