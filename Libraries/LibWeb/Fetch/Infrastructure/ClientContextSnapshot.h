/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/KeepaliveQuotaAccountant.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/MixedContent/ProhibitsMixedSecurityContexts.h>

namespace Web::Fetch::Infrastructure {

enum class HasCrossSiteAncestor : u8 {
    No,
    Yes,
};

// What fetching reads from a request's client.
struct ClientContext {
    enum class GlobalKind : u8 {
        Window,
        Worker,
        Other,
    };

    enum class IsSecureContext : u8 {
        No,
        Yes,
    };

    // Whether the global object is a Window whose navigable is non-null and has no parent.
    enum class IsWindowOfATopLevelNavigable : u8 {
        No,
        Yes,
    };

    // Whether the global object is a Window whose associated Document is loading bypassing the HTTP cache.
    enum class IsLoadingBypassingCache : u8 {
        No,
        Yes,
    };

    URL::Origin origin;
    URL::URL creation_url;
    Optional<URL::URL> top_level_creation_url;
    Optional<URL::Origin> top_level_origin;
    GlobalKind global_kind { GlobalKind::Other };
    IsWindowOfATopLevelNavigable is_window_of_a_top_level_navigable { IsWindowOfATopLevelNavigable::No };
    HasCrossSiteAncestor has_cross_site_ancestor { HasCrossSiteAncestor::No };
    HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability { HTML::CanUseCrossOriginIsolatedAPIs::No };
    IsSecureContext is_secure_context { IsSecureContext::No };
    MixedContent::ProhibitsMixedSecurityContexts prohibits_mixed_security_contexts { MixedContent::ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts };
    IsLoadingBypassingCache is_loading_bypassing_cache { IsLoadingBypassingCache::No };
    double time_origin { 0 };

    // The navigable of the Window that is the global object, if any.
    Optional<HTML::CrossProcessId> navigable_id;

    // The URL a "client" referrer is determined from, or none for no referrer.
    Optional<URL::URL> referrer_source;

    // The URL the content blocker treats as the source of a request that is not for a document.
    URL::URL content_blocker_source_url;

    NonnullRefPtr<KeepaliveQuotaAccountant> keepalive_quota_accountant;
};

// A request's client context as it was when the fetch started.
struct WEB_API ClientContextSnapshot final : public AtomicRefCounted<ClientContextSnapshot>
    , public ClientContext {
    explicit ClientContextSnapshot(ClientContext context)
        : ClientContext(move(context))
    {
    }
};

// What fetching reads from a request's reserved client.
struct ReservedClientContext {
    using IsSecureContext = ClientContext::IsSecureContext;

    enum class TargetBrowsingContext : u8 {
        None,
        TopLevel,
        Nested,
    };

    // The active document of the parent of the target browsing context's navigable.
    struct Parent {
        Optional<URL::Origin> active_document_origin;
        HasCrossSiteAncestor active_document_has_cross_site_ancestor { HasCrossSiteAncestor::No };
    };

    // A reserved client that is an environment but not a settings object.
    struct Environment {
        TargetBrowsingContext target_browsing_context { TargetBrowsingContext::None };
        Optional<Parent> parent;
    };

    // A reserved client that is a settings object.
    struct SettingsObject {
        URL::Origin origin;
        HasCrossSiteAncestor has_cross_site_ancestor { HasCrossSiteAncestor::No };
    };

    URL::URL creation_url;
    Optional<URL::URL> top_level_creation_url;
    Optional<URL::Origin> top_level_origin;
    IsSecureContext is_secure_context { IsSecureContext::No };
    Variant<Environment, SettingsObject> kind;
};

// A request's reserved client context as it was when the reserved client was set.
struct WEB_API ReservedClientContextSnapshot final : public AtomicRefCounted<ReservedClientContextSnapshot>
    , public ReservedClientContext {
    explicit ReservedClientContextSnapshot(ReservedClientContext context)
        : ReservedClientContext(move(context))
    {
    }
};

}
