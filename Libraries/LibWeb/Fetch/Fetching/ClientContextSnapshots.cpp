/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/MixedContent/AbstractOperations.h>

namespace Web::Fetch::Fetching {

GC::Ptr<HTML::EnvironmentSettingsObject> resolve_client(Infrastructure::Request const& request)
{
    auto const& client = request.client();
    if (!client.has_value())
        return nullptr;
    auto const* address = client->get_pointer<HTML::EnvironmentSettingsObjectAddress>();
    if (!address)
        return nullptr;
    auto settings_object = HTML::environment_settings_object_at(*address);
    VERIFY(settings_object);
    return settings_object;
}

RefPtr<HTML::RemoteEnvironmentSettings const> remote_client(Infrastructure::Request const& request)
{
    auto const& client = request.client();
    if (!client.has_value())
        return nullptr;
    if (auto const* settings = client->get_pointer<NonnullRefPtr<HTML::RemoteEnvironmentSettings const>>())
        return *settings;
    return nullptr;
}

Optional<HTML::FetchClient> resolve_fetch_client(Infrastructure::Request const& request)
{
    if (auto settings_object = resolve_client(request))
        return HTML::FetchClient { *settings_object };
    if (auto settings = remote_client(request))
        return HTML::FetchClient { settings.release_nonnull() };
    return {};
}

Infrastructure::Request::ClientType request_client(HTML::FetchClient const& fetch_client)
{
    return fetch_client.visit(
        [](GC::Ref<HTML::EnvironmentSettingsObject> const& settings_object) -> Infrastructure::Request::ClientType {
            return settings_object->address();
        },
        [](NonnullRefPtr<HTML::RemoteEnvironmentSettings const> const& settings) -> Infrastructure::Request::ClientType {
            return settings;
        });
}

// https://w3c.github.io/webappsec-referrer-policy/#determine-requests-referrer
// NB: The steps of the "client" case that read environment, for a client another process hosts.
static Optional<URL::URL> referrer_source_of_client(HTML::SerializedEnvironmentSettingsObject const& environment)
{
    // 2. If environment’s global object is a Window object, then
    if (auto const* window = environment.global.get_pointer<HTML::SerializedWindow>()) {
        // 1. Let document be the associated Document of environment’s global object.
        auto const& document = window->associated_document;

        // 2. If document’s origin is an opaque origin, return no referrer.
        // NB: document's origin is environment's origin.
        if (environment.origin.is_opaque())
            return {};

        // FIXME: 3. While document is an iframe srcdoc document, let document be document’s browsing context’s
        //           browsing context container’s node document.

        // 4. Let referrerSource be document’s URL.
        return document.url;
    }

    // 3. Otherwise, let referrerSource be environment’s creation URL.
    return environment.creation_url;
}

// https://w3c.github.io/webappsec-referrer-policy/#determine-requests-referrer
// NB: The steps of the "client" case that read environment, which is non-null.
static Optional<URL::URL> referrer_source_of_client(HTML::EnvironmentSettingsObject& environment)
{
    // 2. If environment’s global object is a Window object, then
    if (auto const* window = HTML::window_from_global_object(environment.global_object())) {
        // 1. Let document be the associated Document of environment’s global object.
        auto const& document = window->associated_document();

        // 2. If document’s origin is an opaque origin, return no referrer.
        if (document.origin().is_opaque())
            return {};

        // FIXME: 3. While document is an iframe srcdoc document, let document be document’s browsing context’s
        //           browsing context container’s node document.

        // 4. Let referrerSource be document’s URL.
        return document.url();
    }

    // 3. Otherwise, let referrerSource be environment’s creation URL.
    return environment.creation_url;
}

NonnullRefPtr<Infrastructure::ClientContextSnapshot const> snapshot_client_context(HTML::EnvironmentSettingsObject& client)
{
    using Context = Infrastructure::ClientContext;

    auto& global = client.global_object();
    auto const* window = HTML::window_from_global_object(global);
    auto global_kind = Context::GlobalKind::Other;
    if (window)
        global_kind = Context::GlobalKind::Window;
    else if (Bindings::worker_global_scope_from_global_object(global))
        global_kind = Context::GlobalKind::Worker;
    auto navigable = window ? window->navigable() : nullptr;
    auto document = client.responsible_document();

    return make_ref_counted<Infrastructure::ClientContextSnapshot>(Context {
        .origin = client.origin(),
        .creation_url = client.creation_url,
        .top_level_creation_url = client.top_level_creation_url,
        .top_level_origin = client.top_level_origin,
        .global_kind = global_kind,
        .is_window_of_a_top_level_navigable = navigable && navigable->is_top_level_traversable() ? Context::IsWindowOfATopLevelNavigable::Yes : Context::IsWindowOfATopLevelNavigable::No,
        .has_cross_site_ancestor = client.has_cross_site_ancestor() ? Infrastructure::HasCrossSiteAncestor::Yes : Infrastructure::HasCrossSiteAncestor::No,
        .cross_origin_isolated_capability = client.cross_origin_isolated_capability(),
        .is_secure_context = HTML::is_secure_context(client) ? Context::IsSecureContext::Yes : Context::IsSecureContext::No,
        .prohibits_mixed_security_contexts = MixedContent::does_settings_prohibit_mixed_security_contexts(client),
        .is_loading_bypassing_cache = window && window->associated_document().is_loading_bypassing_cache() ? Context::IsLoadingBypassingCache::Yes : Context::IsLoadingBypassingCache::No,
        .time_origin = client.time_origin(),
        .navigable_id = navigable ? navigable->id() : Optional<HTML::CrossProcessId> {},
        .referrer_source = referrer_source_of_client(client),
        .content_blocker_source_url = document ? document->fallback_base_url() : client.api_base_url(),
        .keepalive_quota_accountant = client.keepalive_quota_accountant(),
    });
}

NonnullRefPtr<Infrastructure::ClientContextSnapshot const> snapshot_client_context(HTML::RemoteEnvironmentSettings const& client)
{
    using Context = Infrastructure::ClientContext;

    auto const& settings = client.settings;
    auto const* window = settings.global.get_pointer<HTML::SerializedWindow>();
    auto relevant_settings_object_is_secure_context = window
        ? window->associated_document.relevant_settings_object_is_secure_context
        : settings.global.get<HTML::SerializedWorkerGlobalScope>().relevant_settings_object_is_secure_context;

    // NB: A client another process hosts is never treated as loading bypassing the HTTP cache.
    return make_ref_counted<Infrastructure::ClientContextSnapshot>(Context {
        .origin = settings.origin,
        .creation_url = settings.creation_url,
        .top_level_creation_url = settings.top_level_creation_url,
        .top_level_origin = settings.top_level_origin,
        .global_kind = window ? Context::GlobalKind::Window : Context::GlobalKind::Worker,
        .is_window_of_a_top_level_navigable = window && window->navigable_is_top_level ? Context::IsWindowOfATopLevelNavigable::Yes : Context::IsWindowOfATopLevelNavigable::No,
        .has_cross_site_ancestor = settings.has_cross_site_ancestor ? Infrastructure::HasCrossSiteAncestor::Yes : Infrastructure::HasCrossSiteAncestor::No,
        .cross_origin_isolated_capability = settings.cross_origin_isolated_capability,
        .is_secure_context = relevant_settings_object_is_secure_context ? Context::IsSecureContext::Yes : Context::IsSecureContext::No,
        .prohibits_mixed_security_contexts = settings.prohibits_mixed_security_contexts,
        .time_origin = settings.time_origin,
        .navigable_id = {},
        .referrer_source = referrer_source_of_client(settings),
        .content_blocker_source_url = window ? window->associated_document.url : settings.api_base_url,
        .keepalive_quota_accountant = Infrastructure::KeepaliveQuotaAccountant::create(),
    });
}

static Variant<Infrastructure::ReservedClientContext::Environment, Infrastructure::ReservedClientContext::SettingsObject> reserved_client_kind(HTML::Environment const& reserved_client)
{
    using Context = Infrastructure::ReservedClientContext;

    if (auto const* settings_object = as_if<HTML::EnvironmentSettingsObject>(reserved_client)) {
        return Context::SettingsObject {
            .origin = settings_object->origin(),
            .has_cross_site_ancestor = settings_object->has_cross_site_ancestor() ? Infrastructure::HasCrossSiteAncestor::Yes : Infrastructure::HasCrossSiteAncestor::No,
        };
    }

    Context::Environment environment;
    auto const& target_browsing_context = reserved_client.target_browsing_context;
    if (!target_browsing_context)
        return environment;
    environment.target_browsing_context = target_browsing_context->is_top_level() ? Context::TargetBrowsingContext::TopLevel : Context::TargetBrowsingContext::Nested;

    auto const* document = target_browsing_context->active_document();
    auto navigable = document ? document->navigable() : nullptr;
    if (auto parent = navigable ? navigable->parent() : nullptr) {
        environment.parent = Context::Parent {
            .active_document_origin = parent->active_document_origin(),
            .active_document_has_cross_site_ancestor = parent->active_document_has_cross_site_ancestor() ? Infrastructure::HasCrossSiteAncestor::Yes : Infrastructure::HasCrossSiteAncestor::No,
        };
    }
    return environment;
}

NonnullRefPtr<Infrastructure::ReservedClientContextSnapshot const> snapshot_reserved_client_context(HTML::Environment const& reserved_client)
{
    using Context = Infrastructure::ReservedClientContext;

    return make_ref_counted<Infrastructure::ReservedClientContextSnapshot>(Context {
        .creation_url = reserved_client.creation_url,
        .top_level_creation_url = reserved_client.top_level_creation_url,
        .top_level_origin = reserved_client.top_level_origin,
        .is_secure_context = HTML::is_secure_context(reserved_client) ? Context::IsSecureContext::Yes : Context::IsSecureContext::No,
        .kind = reserved_client_kind(reserved_client),
    });
}

}
