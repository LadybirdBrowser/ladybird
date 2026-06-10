/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/PermissionsAPI/PermissionStatus.h>
#include <LibWeb/PermissionsAPI/PermissionStore.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::PermissionsAPI {

bool is_permission_supported(String const& name)
{
    if (name == "geolocation") {
        return true;
    }
    return false;
}

// https://w3c.github.io/permissions/#dfn-request-permission-to-use
PermissionState request_permission(PermissionDescriptor const& descriptor)
{
    // 1. Let current state be the descriptor's permission state.
    auto current_state = permission_state(descriptor);

    // 2. If current state is not "prompt", return current state and abort these steps.
    if (current_state != PermissionState::Prompt)
        return current_state;

    // FIXME: 3. Ask the user for express permission for the calling algorithm to use the powerful feature described by descriptor.

    // 4. If the user gives express permission to use the powerful feature, set current state to "granted"; otherwise to "denied".
    // The user's interaction may provide new information about the user's intent for the origin.
    if (false) {
        current_state = PermissionState::Granted;
    } else {
        current_state = PermissionState::Denied;
    }

    // 5. Let settings be the current settings object.
    auto& settings = HTML::current_settings_object();

    // 6. Let key be the result of generating a permission key for descriptor with settings's top-level origin and settings's origin.
    VERIFY(settings.top_level_origin.has_value());
    auto key = permission_key_generation_algorithm(settings.top_level_origin.value(), settings.origin());

    // 7. Queue a task on the current settings object's responsible event loop to set a permission store entry with descriptor, key, and current state.
    HTML::queue_global_task(HTML::Task::Source::Permissions, settings.global_object(), GC::create_function(GC::Heap::the(), [descriptor, key, current_state] {
        PermissionStore::the().set_permission_store_entry(descriptor, key, current_state);
    }));

    // 8. Return current state.
    return current_state;
}

// https://w3c.github.io/permissions/#dfn-permission-state
PermissionState permission_state(PermissionDescriptor descriptor, Optional<HTML::EnvironmentSettingsObject&> settings)
{
    // 1. If settings wasn't passed, set it to the current settings object.
    auto& settings_object = settings.has_value() ? settings.value() : HTML::current_settings_object();

    // 2. If settings is a non-secure context, return "denied".
    if (!HTML::is_secure_context(settings_object))
        return PermissionState::Denied;

    // FIXME: 3. Let feature be descriptor's name.

    // FIXME: 4. If there exists a policy-controlled feature for feature and settings' relevant global object has an associated Document run the following step:
    if (false) {
        // 1. Let document be settings' relevant global object's associated Document.
        // 2. If document is not allowed to use feature, return "denied".
    }

    // 5. Let key be the result of generating a permission key for descriptor with settings's top-level origin and settings's origin.
    VERIFY(settings_object.top_level_origin.has_value());
    auto key = permission_key_generation_algorithm(settings_object.top_level_origin.value(), settings_object.origin());

    // 6. Let entry be the result of getting a permission store entry with descriptor and key.
    auto entry = PermissionStore::the().get_permission_store_entry(descriptor, key);

    // 7. If entry is not null, return a PermissionState enum value from entry's state.
    if (entry.has_value())
        return entry.release_value().state;

    // 8. Return the PermissionState enum value that represents the permission state of feature, taking into account any permission state constraints for descriptor's name.
    return PermissionState::Prompt;
}

GC_DEFINE_ALLOCATOR(Permissions);

GC::Ref<Permissions> Permissions::create()
{
    return GC::Heap::the().allocate<Permissions>();
}

Permissions::Permissions() = default;

// https://w3c.github.io/permissions/#query-method
void Permissions::query(JS::Realm& realm, GC::Ref<JS::Object> permission_desc, GC::Ref<WebIDL::Promise> promise)
{
    auto& vm = realm.vm();
    auto& relevant_global_object = HTML::current_global_object();

    // 1. If this's relevant global object is a Window object, then:
    if (auto* window = HTML::window_from_global_object(relevant_global_object)) {
        // 1. If the current settings object's associated Document is not fully active, return a promise rejected with an "InvalidStateError" DOMException.
        if (!window->associated_document().is_fully_active()) {
            auto error = WebIDL::InvalidStateError::create(realm, "The document is not fully active."_utf16);
            WebIDL::reject_promise(realm, promise, error);
            return;
        }
    }

    // 2. Let rootDesc be the object permissionDesc refers to, converted to an IDL value of type PermissionDescriptor.
    // 3. If the conversion throws an exception, return a promise rejected with that exception.
    auto root_desc_or_error = Bindings::convert_to_idl_value_for_permission_descriptor(vm, permission_desc);
    if (root_desc_or_error.is_error()) {
        WebIDL::reject_promise(realm, promise, root_desc_or_error.release_error().value());
        return;
    }
    auto root_desc = root_desc_or_error.release_value();

    // 4. If rootDesc["name"] is not supported, return a promise rejected with a TypeError.
    if (!PermissionsAPI::is_permission_supported(root_desc.name)) {
        auto error = vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, root_desc.name, "PermissionName"sv);
        WebIDL::reject_promise(realm, promise, error.release_error().value());
        return;
    }

    // 5. Let typedDescriptor be the object permissionDesc refers to, converted to an IDL value of rootDesc's name's permission descriptor type.
    // 6. If the conversion throws an exception, return a promise rejected with that exception.
    // FIXME: Support specific permission descriptor types. For now, we only support the base PermissionDescriptor.
    auto typed_descriptor = root_desc;

    // 8. Return promise and continue in parallel:
    // FIXME: Continue in parallel.
    {
        // 1. Let status be create a PermissionStatus with typedDescriptor.
        auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(relevant_global_object);
        VERIFY(global_scope);
        auto status = PermissionStatus::create(global_scope->this_impl(), typed_descriptor);

        // 2. Let query be status's [[query]] internal slot.
        auto const& query = status->query();

        // 3. Run query's name's permission query algorithm, passing query and status.
        // FIXME: For now, only the base PermissionDescriptor is supported so there is only one permission query algorithm.
        permission_query_algorithm(query, status);

        // 4. Queue a global task on the permissions task source with this's relevant global object to resolve promise with status.
        HTML::queue_global_task(HTML::Task::Source::Permissions, relevant_global_object, GC::create_function(GC::Heap::the(), [relevant_global_object = GC::Ref(relevant_global_object), promise, status]() mutable {
            auto& realm = HTML::relevant_realm(relevant_global_object);
            HTML::TemporaryExecutionContext execution_context { realm };
            WebIDL::resolve_promise(realm, promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, status));
        }));
    }
}

// https://w3c.github.io/permissions/#dfn-getting-the-current-permission-state
PermissionState get_current_permission_state(String const& name, Optional<HTML::EnvironmentSettingsObject&> settings)
{
    // 1. Let descriptor be a newly-created PermissionDescriptor with name initialized to name.
    PermissionDescriptor descriptor { name };

    // 2. Return the permission state of descriptor with settings.
    return permission_state(descriptor, settings);
}

// https://w3c.github.io/permissions/#dfn-permission-query-algorithm
void permission_query_algorithm(PermissionDescriptor const& permission_desc, PermissionStatus& status)
{
    // 1. Set status's state to permissionDesc's permission state.
    status.set_state(permission_state(permission_desc));
}

}
