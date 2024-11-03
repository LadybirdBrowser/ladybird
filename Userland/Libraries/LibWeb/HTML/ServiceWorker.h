/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ServiceWorkerPrototype.h>
#include <LibWeb/DOM/EventTarget.h>

#define ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(E)  \
    E(onstatechange, HTML::EventNames::statechange) \
    E(onerror, HTML::EventNames::error)

namespace Web::HTML {

class ServiceWorker : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ServiceWorker, DOM::EventTarget);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ServiceWorker> create(JS::Realm& realm);

    virtual ~ServiceWorker() override;

    String script_url() const { return m_script_url; }
    Bindings::ServiceWorkerState service_worker_state() const { return m_state; }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    ServiceWorker(JS::Realm&, String script_url);

    virtual void initialize(JS::Realm&) override;

    String m_script_url;
    Bindings::ServiceWorkerState m_state { Bindings::ServiceWorkerState::Parsed };
};

}
