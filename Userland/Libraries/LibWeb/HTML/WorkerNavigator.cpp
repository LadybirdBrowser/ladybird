/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WorkerNavigatorPrototype.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerNavigator.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(WorkerNavigator);

JS::NonnullGCPtr<WorkerNavigator> WorkerNavigator::create(WorkerGlobalScope& global_scope)
{
    return global_scope.heap().allocate<WorkerNavigator>(global_scope.realm(), global_scope);
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

JS::NonnullGCPtr<MediaCapabilitiesAPI::MediaCapabilities> WorkerNavigator::media_capabilities()
{
    if (!m_media_capabilities)
        m_media_capabilities = heap().allocate<MediaCapabilitiesAPI::MediaCapabilities>(realm(), realm());
    return *m_media_capabilities;
}

JS::NonnullGCPtr<ServiceWorkerContainer> WorkerNavigator::service_worker()
{
    if (!m_service_worker_container)
        m_service_worker_container = heap().allocate<ServiceWorkerContainer>(realm(), realm());
    return *m_service_worker_container;
}

}
