/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/PermissionStore.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::PermissionsAPI {

Optional<Web::Permissions::PermissionName> permission_name_from(Utf16View name)
{
    if (name == "geolocation"_utf16)
        return Web::Permissions::PermissionName::Geolocation;
    if (name == "microphone"_utf16)
        return Web::Permissions::PermissionName::Microphone;
    if (name == "notifications"_utf16)
        return Web::Permissions::PermissionName::Notifications;
    return {};
}

static Optional<PermissionsPolicy::PolicyControlledFeature> policy_controlled_feature_for(Web::Permissions::PermissionName name)
{
    switch (name) {
    case Web::Permissions::PermissionName::Geolocation:
        return PermissionsPolicy::PolicyControlledFeature::Geolocation;
    case Web::Permissions::PermissionName::Microphone:
        return PermissionsPolicy::PolicyControlledFeature::Microphone;
    case Web::Permissions::PermissionName::Notifications:
        // Notifications is a powerful feature, but not a policy-controlled one.
        return {};
    }
    VERIFY_NOT_REACHED();
}

static PermissionState permission_state_from(Web::Permissions::EffectiveState state)
{
    switch (state) {
    case Web::Permissions::EffectiveState::Granted:
        return PermissionState::Granted;
    case Web::Permissions::EffectiveState::Denied:
        return PermissionState::Denied;
    case Web::Permissions::EffectiveState::Prompt:
        return PermissionState::Prompt;
    }
    VERIFY_NOT_REACHED();
}

// Steps 6 and 7 of https://w3c.github.io/permissions/#dfn-request-permission-to-use
static void store_permission_decision(Web::Permissions::PermissionName name, HTML::EnvironmentSettingsObject& settings, PermissionState current_state)
{
    // 6. Let key be the result of generating a permission key for descriptor with settings's top-level origin and settings's origin.
    VERIFY(settings.top_level_origin.has_value());
    auto key = permission_key_generation_algorithm(settings.top_level_origin.value(), settings.origin());

    // AD-HOC: Our permission state is backed by PermissionStore, so update it before returning to make the new current
    //         state observable to callers that continue synchronously.
    PermissionStore::the().set_permission_store_entry(name, key, current_state);

    // 7. Queue a task on the current settings object's responsible event loop to set a permission store entry with descriptor, key, and current state.
    HTML::queue_global_task(HTML::Task::Source::Permissions, settings.global_object(), GC::create_function(GC::Heap::the(), GC::captures(name, key, current_state), [](auto& name, auto& key, auto& current_state) {
        PermissionStore::the().set_permission_store_entry(name, key, current_state);
    }));
}

// https://w3c.github.io/permissions/#dfn-request-permission-to-use
void request_permission_with_user_prompt(Web::Permissions::PermissionName name, HTML::EnvironmentSettingsObject& settings, PermissionDecisionCallback on_decided)
{
    // 1. Let current state be the descriptor's permission state.
    auto current_state = permission_state(name, settings);

    // 2. If current state is not "prompt", return current state and abort these steps.
    if (current_state != PermissionState::Prompt) {
        on_decided->function()(current_state);
        return;
    }

    // 3. Ask the user for express permission for the calling algorithm to use the powerful feature described by descriptor.
    // NB: Only windows have a page to ask through; a worker's request is denied.
    auto* window = HTML::window_from_global_object(settings.global_object());
    if (!window) {
        on_decided->function()(PermissionState::Denied);
        return;
    }

    auto settle = GC::create_function(GC::Heap::the(), GC::captures(name, GC::Ref(settings), on_decided), [](auto& name, auto& settings, auto& on_decided, Web::Permissions::EffectiveState answer) {
        // 4. If the user gives express permission to use the powerful feature, set current state to "granted"; otherwise to "denied".
        //    The user's interaction may provide new information about the user's intent for the origin.
        // AD-HOC: A prompt the user dismisses or ignores leaves the permission at "prompt", rather than storing "denied";
        //         only the calling algorithm sees "denied" then, so the site may ask again.
        auto current_state = answer == Web::Permissions::EffectiveState::Granted ? PermissionState::Granted : PermissionState::Denied;

        // 5. Let settings be the current settings object.
        // NB: The user's answer arrives outside any script, so the requester's settings object was captured instead.
        // 6. Let key be the result of generating a permission key for descriptor with settings's top-level origin and settings's origin.
        // 7. Queue a task on the current settings object's responsible event loop to set a permission store entry with descriptor, key, and current state.
        // NB: The UI process stores the user's answer, and sends the snapshot that reflects it ahead of the answer itself.
        //     It can't key an answer for an opaque top-level origin (a file: page's), so that answer is kept here, for
        //     as long as the origin lives.
        if (settings->top_level_origin->is_opaque() && answer != Web::Permissions::EffectiveState::Prompt)
            store_permission_decision(name, settings, current_state);

        // 8. Return current state.
        on_decided->function()(current_state);
    });

    window->page().request_permission(window->associated_document().id(), Utf16String::from_utf8(Web::Permissions::to_string(name)), settle);
}

// https://w3c.github.io/permissions/#dfn-permission-state
PermissionState permission_state(Web::Permissions::PermissionName name, Optional<HTML::EnvironmentSettingsObject&> settings)
{
    // 1. If settings wasn't passed, set it to the current settings object.
    auto& settings_object = settings.has_value() ? settings.value() : HTML::current_settings_object();

    // 2. If settings is a non-secure context, return "denied".
    if (!HTML::is_secure_context(settings_object))
        return PermissionState::Denied;

    // 3. Let feature be descriptor's name.
    auto feature = name;

    // 4. If there exists a policy-controlled feature for feature and settings' relevant global object has an associated Document run the following step:
    auto policy_controlled_feature = policy_controlled_feature_for(feature);
    if (auto* window = HTML::window_from_global_object(settings_object.global_object()); policy_controlled_feature.has_value() && window) {
        // 1. Let document be settings' relevant global object's associated Document.
        auto& document = window->associated_document();

        // 2. If document is not allowed to use feature, return "denied".
        if (!document.is_allowed_to_use_feature(*policy_controlled_feature))
            return PermissionState::Denied;
    }

    // 5. Let key be the result of generating a permission key for descriptor with settings's top-level origin and settings's origin.
    VERIFY(settings_object.top_level_origin.has_value());
    auto key = permission_key_generation_algorithm(settings_object.top_level_origin.value(), settings_object.origin());

    // 6. Let entry be the result of getting a permission store entry with descriptor and key.
    auto entry = PermissionStore::the().get_permission_store_entry(feature, key);

    // 7. If entry is not null, return a PermissionState enum value from entry's state.
    if (entry.has_value())
        return entry.release_value().state;

    // 8. Return the PermissionState enum value that represents the permission state of feature, taking into account any permission state constraints for descriptor's name.
    auto& page = Bindings::principal_host_defined_page(settings_object.realm());
    return permission_state_from(PermissionStore::the().state_for(feature, key, page));
}

extern "C" {
Permissions* web_rust_permissions_api_create_permissions(JSVM*);
u8 web_permissions_api_permission_state(HTML::EnvironmentSettingsObject*, u8 name);
void web_permissions_api_request_permission_to_use(HTML::EnvironmentSettingsObject*, u8 name, GC::Cell* cell, void (*steps)(JSVM*, void*, u8));
}

static_assert(to_underlying(Web::Permissions::PermissionName::Geolocation) == 0);
static_assert(to_underlying(Web::Permissions::PermissionName::Microphone) == 1);
static_assert(to_underlying(Web::Permissions::PermissionName::Notifications) == 2);
static_assert(to_underlying(PermissionState::Granted) == 0);
static_assert(to_underlying(PermissionState::Denied) == 1);
static_assert(to_underlying(PermissionState::Prompt) == 2);

GC::Ref<Permissions> Permissions::create()
{
    return *web_rust_permissions_api_create_permissions(JS::HostABI::vm_to_abi(JS::VM::the()));
}

u8 web_permissions_api_permission_state(HTML::EnvironmentSettingsObject* settings, u8 name)
{
    return to_underlying(permission_state(static_cast<Web::Permissions::PermissionName>(name), *settings));
}

void web_permissions_api_request_permission_to_use(HTML::EnvironmentSettingsObject* settings, u8 name, GC::Cell* cell, void (*steps)(JSVM*, void*, u8))
{
    request_permission_with_user_prompt(static_cast<Web::Permissions::PermissionName>(name), *settings, GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *cell }, steps), [](auto& cell, auto& steps, PermissionState state) {
        steps(JS::HostABI::vm_to_abi(JS::VM::the()), cell.ptr(), to_underlying(state));
    }));
}

}
