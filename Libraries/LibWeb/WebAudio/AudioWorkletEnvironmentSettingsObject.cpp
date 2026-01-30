/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/WebAudio/AudioWorkletEnvironmentSettingsObject.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioWorkletEnvironmentSettingsObject);

AudioWorkletEnvironmentSettingsObject::AudioWorkletEnvironmentSettingsObject(
    NonnullOwnPtr<JS::ExecutionContext> execution_context,
    GC::Ref<AudioWorkletGlobalScope> global_scope,
    URL::URL global_scope_url,
    URL::Origin origin,
    bool outside_settings_has_cross_site_ancestor,
    GC::Ref<HTML::PolicyContainer> policy_container,
    HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability,
    double time_origin)
    : HTML::EnvironmentSettingsObject(move(execution_context))
    , m_global_scope(global_scope)
    , m_global_scope_url(move(global_scope_url))
    , m_origin(move(origin))
    , m_outside_settings_has_cross_site_ancestor(outside_settings_has_cross_site_ancestor)
    , m_policy_container(policy_container)
    , m_cross_origin_isolated_capability(cross_origin_isolated_capability)
    , m_time_origin(time_origin)
{
}

GC::Ref<AudioWorkletEnvironmentSettingsObject> AudioWorkletEnvironmentSettingsObject::setup(GC::Ref<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, HTML::SerializedEnvironmentSettingsObject const& outside_settings, URL::URL const& global_scope_url)
{
    auto realm = execution_context->realm;
    VERIFY(realm);

    auto& global_scope = as<AudioWorkletGlobalScope>(realm->global_object());

    auto policy_container = HTML::create_a_policy_container_from_serialized_policy_container(realm->heap(), outside_settings.policy_container);

    auto settings_object = realm->create<AudioWorkletEnvironmentSettingsObject>(
        move(execution_context),
        global_scope,
        global_scope_url,
        outside_settings.origin,
        outside_settings.has_cross_site_ancestor,
        policy_container,
        outside_settings.cross_origin_isolated_capability,
        outside_settings.time_origin);

    settings_object->target_browsing_context = nullptr;
    settings_object->top_level_origin = outside_settings.top_level_origin;

    auto intrinsics = realm->create<Bindings::Intrinsics>(*realm);
    auto host_defined = make<Bindings::PrincipalHostDefined>(settings_object, intrinsics, page);
    realm->set_host_defined(move(host_defined));

    global_scope.initialize_web_interfaces();

    return settings_object;
}

URL::URL AudioWorkletEnvironmentSettingsObject::api_base_url() const
{
    return m_global_scope_url;
}

URL::Origin AudioWorkletEnvironmentSettingsObject::origin() const
{
    return m_origin;
}

bool AudioWorkletEnvironmentSettingsObject::has_cross_site_ancestor() const
{
    return m_outside_settings_has_cross_site_ancestor;
}

GC::Ref<HTML::PolicyContainer> AudioWorkletEnvironmentSettingsObject::policy_container() const
{
    return m_policy_container;
}

HTML::CanUseCrossOriginIsolatedAPIs AudioWorkletEnvironmentSettingsObject::cross_origin_isolated_capability() const
{
    return m_cross_origin_isolated_capability;
}

double AudioWorkletEnvironmentSettingsObject::time_origin() const
{
    return m_time_origin;
}

void AudioWorkletEnvironmentSettingsObject::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
    visitor.visit(m_policy_container);
}

}
