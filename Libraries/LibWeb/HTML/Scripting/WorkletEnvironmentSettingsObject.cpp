/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/WorkletEnvironmentSettingsObject.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkletEnvironmentSettingsObject);

// https://html.spec.whatwg.org/multipage/worklets.html#set-up-a-worklet-environment-settings-object
GC::Ref<WorkletEnvironmentSettingsObject> WorkletEnvironmentSettingsObject::setup(GC::Ref<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, GC::Ref<WorkletGlobalScope> global_scope, EnvironmentSettingsObject& outside_settings)
{
    // 1. Let inheritedAPIBaseURL be outsideSettings's API base URL.
    auto inherited_api_base_url = outside_settings.api_base_url();

    // 2. Let inheritedPolicyContainer be a clone of outsideSettings's policy container.
    auto inherited_policy_container = outside_settings.policy_container()->clone(GC::Heap::the());

    // 3. Let realm be the value of realmExecutionContext's Realm component.
    auto realm = execution_context->realm;
    VERIFY(realm);

    // 4. Let workletGlobalScope be realm's global object.
    // NOTE: Passed in by the caller, which just created the realm around it.

    // 5. Let settings object be a new environment settings object whose algorithms are defined as follows:
    //    - The realm execution context: realmExecutionContext.
    //    - The module map: workletGlobalScope's module map. (Our module map lives on the settings object itself.)
    //    - The API base URL: inheritedAPIBaseURL.
    //    - The origin: outsideSettings's origin.
    //    - The policy container: inheritedPolicyContainer.
    //    - The cross-origin isolated capability: outsideSettings's cross-origin isolated capability.
    //    - The time origin: outsideSettings's time origin.
    auto settings_object = realm->create<WorkletEnvironmentSettingsObject>(move(execution_context), global_scope,
        move(inherited_api_base_url), outside_settings.origin(), outside_settings.has_cross_site_ancestor(),
        inherited_policy_container, outside_settings.cross_origin_isolated_capability(), outside_settings.time_origin());
    settings_object->target_browsing_context = nullptr;

    // 6. Set settings object's id to a new unique opaque string, creation URL to inheritedAPIBaseURL, top-level
    //    creation URL to null, target browsing context to null, and active service worker to null.
    settings_object->creation_url = settings_object->m_api_base_url;
    // FIXME: Setting top-level origin to the outside settings' value (rather than null) matches what
    //        WorkerEnvironmentSettingsObject does; it needs to be non-null for network partition keys.
    settings_object->top_level_origin = outside_settings.top_level_origin;

    // 7. Set realm's [[HostDefined]] field to settings object.
    auto intrinsics = realm->create<Bindings::Intrinsics>(*realm);
    realm->set_host_defined(Bindings::create_principal_host_defined(settings_object, intrinsics, page));
    Bindings::cache_global_object_wrapper(*realm);

    global_scope->set_settings_object({}, *settings_object);
    global_scope->set_url(settings_object->m_api_base_url);

    // AD-HOC: Worklet global scopes are execution-ready as soon as the realm exists; no script fetch
    //         precedes readiness (module fetching happens per addModule() call).
    settings_object->execution_ready = true;

    // 8. Return settings object.
    return settings_object;
}

WorkletEnvironmentSettingsObject::WorkletEnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> execution_context, GC::Ref<WorkletGlobalScope> global_scope, URL::URL api_base_url, URL::Origin origin, bool has_cross_site_ancestor, GC::Ref<PolicyContainer> policy_container, CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability, double time_origin)
    : EnvironmentSettingsObject(move(execution_context))
    , m_global_scope(global_scope)
    , m_api_base_url(move(api_base_url))
    , m_origin(move(origin))
    , m_has_cross_site_ancestor(has_cross_site_ancestor)
    , m_policy_container(policy_container)
    , m_cross_origin_isolated_capability(cross_origin_isolated_capability)
    , m_time_origin(time_origin)
{
}

GC::Ref<PolicyContainer> WorkletEnvironmentSettingsObject::policy_container() const
{
    return m_policy_container;
}

void WorkletEnvironmentSettingsObject::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
    visitor.visit(m_policy_container);
}

}
