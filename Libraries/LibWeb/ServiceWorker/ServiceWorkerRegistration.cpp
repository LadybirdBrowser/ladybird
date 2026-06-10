/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerRegistration);

ServiceWorkerRegistration::ServiceWorkerRegistration(Registration const& registration)
    : DOM::EventTarget()
    , m_registration(registration)
{
}

void ServiceWorkerRegistration::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_installing);
    visitor.visit(m_waiting);
    visitor.visit(m_active);
}

GC::Ref<ServiceWorkerRegistration> ServiceWorkerRegistration::create(Registration const& registration)
{
    return GC::Heap::the().allocate<ServiceWorkerRegistration>(registration);
}

}
