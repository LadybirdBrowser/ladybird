/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/PermissionsAPI/PermissionStatus.h>
#include <LibWeb/PermissionsAPI/PermissionStore.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::PermissionsAPI {

bool is_permission_supported(String const&)
{
    // FIXME: Actually support permissions
    return false;
}

// https://w3c.github.io/permissions/#dfn-permission-state
Bindings::PermissionState permission_state(Bindings::PermissionDescriptor descriptor, Optional<HTML::EnvironmentSettingsObject&> settings)
{
    // 1. If settings wasn't passed, set it to the current settings object.
    auto& settings_object = settings.has_value() ? settings.value() : HTML::current_settings_object();

    // 2. If settings is a non-secure context, return "denied".
    if (!HTML::is_secure_context(settings_object))
        return Bindings::PermissionState::Denied;

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
    return Bindings::PermissionState::Prompt;
}

GC_DEFINE_ALLOCATOR(Permissions);

GC::Ref<Permissions> Permissions::create(JS::Realm& realm)
{
    return realm.create<Permissions>(realm);
}

Permissions::Permissions(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void Permissions::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Permissions);
    Base::initialize(realm);
}

// https://w3c.github.io/permissions/#query-method
GC::Ref<Web::WebIDL::Promise> Permissions::query(JS::Value permission_desc)
{
    auto& realm = this->realm();

    auto& relevant_global_object = HTML::relevant_global_object(*this);

    // 1. If this's relevant global object is a Window object, then:
    if (auto* window = as_if<HTML::Window>(relevant_global_object)) {
        // 1. If the current settings object's associated Document is not fully active, return a promise rejected with an "InvalidStateError" DOMException.
        if (!window->associated_document().is_fully_active()) {
            auto error = WebIDL::InvalidStateError::create(realm, "The document is not fully active."_utf16);
            return WebIDL::create_rejected_promise_from_exception(realm, error);
        }
    }

    // 2. Let rootDesc be the object permissionDesc refers to, converted to an IDL value of type PermissionDescriptor.
    // 3. If the conversion throws an exception, return a promise rejected with that exception.
    auto root_desc_or_error = Bindings::convert_to_idl_value_for_permission_descriptor(vm(), permission_desc);
    if (root_desc_or_error.is_error()) {
        return WebIDL::create_rejected_promise_from_exception(realm, root_desc_or_error.release_error());
    }
    auto root_desc = root_desc_or_error.release_value();

    // 4. If rootDesc["name"] is not supported, return a promise rejected with a TypeError.
    if (!is_permission_supported(root_desc.name)) {
        auto error = vm().throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, root_desc.name, "PermissionName"sv);
        return WebIDL::create_rejected_promise_from_exception(realm, error.release_error());
    }

    // 5. Let typedDescriptor be the object permissionDesc refers to, converted to an IDL value of rootDesc's name's permission descriptor type.
    // 6. If the conversion throws an exception, return a promise rejected with that exception.
    // FIXME: Support specific permission descriptor types. For now, we only support the base PermissionDescriptor.
    auto typed_descriptor = root_desc;

    // 7. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 8. Return promise and continue in parallel:
    // FIXME: Continue in parallel.
    {
        // 1. Let status be create a PermissionStatus with typedDescriptor.
        auto status = PermissionStatus::create(realm, typed_descriptor);

        // 2. Let query be status's [[query]] internal slot.
        auto const& query = status->query();

        // 3. Run query's name's permission query algorithm, passing query and status.
        // FIXME: For now, only the base PermissionDescriptor is supported so there is only one permission query algorithm.
        permission_query_algorithm(query, status);

        // 4. Queue a global task on the permissions task source with this's relevant global object to resolve promise with status.
        HTML::queue_global_task(HTML::Task::Source::Permissions, relevant_global_object, GC::create_function(realm.heap(), [&realm, promise, status]() mutable {
            HTML::TemporaryExecutionContext execution_context { realm };
            WebIDL::resolve_promise(realm, promise, status);
        }));
    }

    return promise;
}

// https://w3c.github.io/permissions/#dfn-getting-the-current-permission-state
Bindings::PermissionState get_current_permission_state(String const& name, Optional<HTML::EnvironmentSettingsObject&> settings)
{
    // 1. Let descriptor be a newly-created PermissionDescriptor with name initialized to name.
    Bindings::PermissionDescriptor descriptor { name };

    // 2. Return the permission state of descriptor with settings.
    return permission_state(descriptor, settings);
}

// https://w3c.github.io/permissions/#dfn-permission-query-algorithm
void permission_query_algorithm(Bindings::PermissionDescriptor const& permission_desc, PermissionStatus& status)
{
    // 1. Set status's state to permissionDesc's permission state.
    status.set_state(permission_state(permission_desc));
}

}
