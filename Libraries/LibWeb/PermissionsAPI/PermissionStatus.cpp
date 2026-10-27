/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Types.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/PermissionsAPI/PermissionStatus.h>
#include <LibWeb/PermissionsAPI/PermissionStore.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::PermissionsAPI {

GC_DEFINE_ALLOCATOR(PermissionStatus);

PermissionStatus::PermissionStatus(GC::Ref<DOM::EventTarget> relevant_global_object, Utf16String const& name,
    PermissionDescriptor query)
    : DOM::EventTarget()
    , m_name(name)
    , m_query(move(query))
    , m_global_object(relevant_global_object)
{
}

void PermissionStatus::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_object);
}

GC::Ref<PermissionStatus> PermissionStatus::create(GC::Ref<DOM::EventTarget> relevant_global_object, PermissionDescriptor permission_desc)
{
    // 1. Let name be permissionDesc's name.
    auto name = permission_desc.name;

    // 2. Assert: The feature identified by name is supported by the user agent.
    VERIFY(is_permission_supported(name));

    // 3. Let status be a new instance of the permission result type identified by name:
    // FIXME: A permission result type is a PermissionStatus or one of its subtypes.
    // If unspecified, this defaults to PermissionStatus. No subtypes exist yet in Ladybird.
    // 1. Initialize status's [[query]] internal slot to permissionDesc.
    // 2. Initialize status's name to name.
    auto status = GC::Heap::the().allocate<PermissionStatus>(relevant_global_object, name, move(permission_desc));

    // NB: The store tells every status when a permission changes, and the global keeps a status alive while it has a
    //     change listener.
    PermissionStore::the().register_status(status);
    if (auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(status->relevant_global_object()))
        global_scope->register_permission_status(status);

    // 4. Return status.
    return status;
}

PermissionState PermissionStatus::state() const
{
    return m_state;
}

void PermissionStatus::update_from_store()
{
    auto& global = relevant_global_object();

    // A document that isn't fully active sees no change until it is again.
    if (auto* window = HTML::window_from_global_object(global); window && !window->associated_document().is_fully_active())
        return;

    auto state = permission_state(m_query, HTML::relevant_settings_object(global));
    if (state == m_state)
        return;

    // NB: The state is read again when the task runs, so a status that heard of several changes ends with the last.
    HTML::queue_global_task(HTML::Task::Source::Permissions, global, GC::create_function(heap(), [this] {
        auto& global = relevant_global_object();
        auto state = permission_state(m_query, HTML::relevant_settings_object(global));
        if (state == m_state)
            return;
        m_state = state;
        dispatch_event(DOM::Event::create(global, HTML::EventNames::change));
    }));
}

JS::Object& PermissionStatus::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

// https://w3c.github.io/permissions/#dom-permissionstatus-onchange
void PermissionStatus::set_onchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://w3c.github.io/permissions/#dom-permissionstatus-onchange
GC::Ptr<WebIDL::CallbackType> PermissionStatus::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

}
