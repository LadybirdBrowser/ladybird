/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerNavigator.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerNavigator);

GC::Ref<WorkerNavigator> WorkerNavigator::create(WorkerGlobalScope& global_scope)
{
    return GC::Heap::the().allocate<WorkerNavigator>(global_scope);
}

WorkerNavigator::WorkerNavigator(WorkerGlobalScope& global_scope)
    : m_global_scope(global_scope)
{
}

WorkerNavigator::~WorkerNavigator() = default;

void WorkerNavigator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
    visitor.visit(m_media_capabilities);
    visitor.visit(m_serial);
    visitor.visit(m_service_worker_container);
    visitor.visit(m_permissions);
}

EnvironmentSettingsObject& WorkerNavigator::navigator_storage_settings_object() const
{
    return relevant_settings_object(*m_global_scope);
}

GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> WorkerNavigator::media_capabilities()
{
    if (!m_media_capabilities)
        m_media_capabilities = MediaCapabilitiesAPI::MediaCapabilities::create();
    return *m_media_capabilities;
}

GC::Ref<Serial::Serial> WorkerNavigator::serial()
{
    if (!m_serial)
        m_serial = GC::Heap::the().allocate<Serial::Serial>();
    return *m_serial;
}

GC::Ref<ServiceWorker::ServiceWorkerContainer> WorkerNavigator::service_worker()
{
    if (!m_service_worker_container)
        m_service_worker_container = ServiceWorker::ServiceWorkerContainer::create(relevant_settings_object(*m_global_scope));
    return *m_service_worker_container;
}

GC::Ref<PermissionsAPI::Permissions> WorkerNavigator::permissions()
{
    if (!m_permissions)
        m_permissions = PermissionsAPI::Permissions::create();
    return *m_permissions;
}

}
