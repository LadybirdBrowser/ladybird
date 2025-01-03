/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworkerregistration-interface
class ServiceWorkerRegistration : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ServiceWorkerRegistration, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(ServiceWorkerRegistration);

public:
    [[nodiscard]] static GC::Ref<ServiceWorkerRegistration> create(JS::Realm& realm, Registration const& registration);

    Registration const& registration() { return m_registration; }

    GC::Ptr<ServiceWorker> installing() const { return m_installing; }
    void set_installing(GC::Ptr<ServiceWorker> installing) { m_installing = installing; }

    GC::Ptr<ServiceWorker> waiting() const { return m_waiting; }
    void set_waiting(GC::Ptr<ServiceWorker> waiting) { m_waiting = waiting; }

    GC::Ptr<ServiceWorker> active() const { return m_active; }
    void set_active(GC::Ptr<ServiceWorker> active) { m_active = active; }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-scope
    String scope() const { return m_registration.scope_url().serialize(); }

    // https://w3c.github.io/ServiceWorker/#dom-serviceworkerregistration-updateviacache
    Bindings::ServiceWorkerUpdateViaCache update_via_cache() const { return m_registration.update_via_cache(); }

    explicit ServiceWorkerRegistration(JS::Realm&, Registration const&);
    virtual ~ServiceWorkerRegistration() override = default;

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    Registration const& m_registration;
    GC::Ptr<ServiceWorker> m_installing;
    GC::Ptr<ServiceWorker> m_waiting;
    GC::Ptr<ServiceWorker> m_active;
};

}
