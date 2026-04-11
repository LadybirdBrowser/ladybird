/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Types.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PermissionStatus.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/PermissionsAPI/PermissionStatus.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::PermissionsAPI {

GC_DEFINE_ALLOCATOR(PermissionStatus);

void PermissionStatus::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PermissionStatus);
    Base::initialize(realm);
}

PermissionStatus::PermissionStatus(JS::Realm& realm, String const& name, Bindings::PermissionDescriptor const& query)
    : DOM::EventTarget(realm)
    , m_name(name)
    , m_query(query)
{
}

GC::Ref<PermissionStatus> PermissionStatus::create(JS::Realm& realm, Bindings::PermissionDescriptor permission_desc)
{
    // 1. Let name be permissionDesc's name.
    String name = permission_desc.name;

    // 2. Assert: The feature identified by name is supported by the user agent.
    VERIFY(is_permission_supported(name));

    // 3. Let status be a new instance of the permission result type identified by name:
    // FIXME: A permission result type is a PermissionStatus or one of its subtypes.
    // If unspecified, this defaults to PermissionStatus. No subtypes exist yet in Ladybird.
    // 1. Initialize status's [[query]] internal slot to permissionDesc.
    // 2. Initialize status's name to name.
    auto status = realm.create<PermissionStatus>(realm, name, permission_desc);

    // 4. Return status.
    return status;
}

// https://w3c.github.io/permissions/#dfn-permissionstatus-update-steps
void PermissionStatus::update_steps()
{
    auto& relevant_global_object = HTML::relevant_global_object(*this);
    // 1. If this's relevant global object is a Window object, then:
    if (auto* window = as_if<HTML::Window>(relevant_global_object)) {
        // 1. Let document be status's relevant global object's associated Document.
        auto const& document = window->associated_document();
        // 2. If document is null or document is not fully active, terminate this algorithm.
        if (!document.is_fully_active()) {
            return;
        }
    }

    // 2. Let query be status's [[query]] internal slot.
    auto query = m_query;

    // 3. Run query's name's permission query algorithm, passing query and status.
    permission_query_algorithm(query, *this);

    // 4. Queue a task on the permissions task source to fire an event named change at status.
    HTML::queue_a_task(
        HTML::Task::Source::Permissions, nullptr, nullptr, GC::create_function(realm().heap(), [this] {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::change));
        }));
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
