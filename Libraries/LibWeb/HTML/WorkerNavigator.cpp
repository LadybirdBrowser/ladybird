/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WorkerNavigatorPrototype.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerNavigator.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerNavigator);

GC::Ref<WorkerNavigator> WorkerNavigator::create(WorkerGlobalScope& global_scope)
{
    return global_scope.realm().create<WorkerNavigator>(global_scope);
}

WorkerNavigator::WorkerNavigator(WorkerGlobalScope& global_scope)
    : PlatformObject(global_scope.realm())
{
}

WorkerNavigator::~WorkerNavigator() = default;

void WorkerNavigator::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WorkerNavigator);
}

void WorkerNavigator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_capabilities);
    visitor.visit(m_service_worker_container);
}

GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> WorkerNavigator::media_capabilities()
{
    if (!m_media_capabilities)
        m_media_capabilities = realm().create<MediaCapabilitiesAPI::MediaCapabilities>(realm());
    return *m_media_capabilities;
}

GC::Ref<ServiceWorker::ServiceWorkerContainer> WorkerNavigator::service_worker()
{
    if (!m_service_worker_container)
        m_service_worker_container = realm().create<ServiceWorker::ServiceWorkerContainer>(realm());
    return *m_service_worker_container;
}

}
