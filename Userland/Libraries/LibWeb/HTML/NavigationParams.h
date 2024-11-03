/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibURL/Origin.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicyEnforcementResult.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigation-params
struct NavigationParams : JS::Cell {
    JS_CELL(NavigationParams, JS::Cell);
    JS_DECLARE_ALLOCATOR(NavigationParams);

    // null or a navigation ID
    Optional<String> id;

    // the navigable to be navigated
    JS::GCPtr<Navigable> navigable;

    // null or a request that started the navigation
    JS::GCPtr<Fetch::Infrastructure::Request> request;

    // a response that ultimately was navigated to (potentially a network error)
    JS::GCPtr<Fetch::Infrastructure::Response> response;

    // null or a fetch controller
    JS::GCPtr<Fetch::Infrastructure::FetchController> fetch_controller { nullptr };

    // null or an algorithm accepting a Document, once it has been created
    Function<void(DOM::Document&)> commit_early_hints { nullptr };

    // an opener policy enforcement result, used for reporting and potentially for causing a browsing context group switch
    OpenerPolicyEnforcementResult coop_enforcement_result;

    // null or an environment reserved for the new Document
    Fetch::Infrastructure::Request::ReservedClientType reserved_environment;

    // an origin to use for the new Document
    URL::Origin origin;

    // a policy container to use for the new Document
    PolicyContainer policy_container;

    // a sandboxing flag set to impose on the new Document
    SandboxingFlagSet final_sandboxing_flag_set = {};

    // an opener policy to use for the new Document
    OpenerPolicy opener_policy;

    // FIXME: a NavigationTimingType used for creating the navigation timing entry for the new Document

    // a URL or null used to populate the new Document's about base URL
    Optional<URL::URL> about_base_url;

    void visit_edges(Visitor& visitor) override;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#non-fetch-scheme-navigation-params
struct NonFetchSchemeNavigationParams : JS::Cell {
    JS_CELL(NonFetchSchemeNavigationParams, JS::Cell);
    JS_DECLARE_ALLOCATOR(NonFetchSchemeNavigationParams);

    // null or a navigation ID
    Optional<String> id;

    // the navigable to be navigated
    JS::GCPtr<Navigable> navigable;

    // a URL
    URL::URL url;

    // the target snapshot params's sandboxing flags present during navigation
    SandboxingFlagSet target_snapshot_sandboxing_flags = {};

    // a copy of the source snapshot params's has transient activation boolean present during activation
    bool source_snapshot_has_transient_activation = { false };

    // an origin possibly for use in a user-facing prompt to confirm the invocation of an external software package
    URL::Origin initiator_origin;

    // FIXME: a NavigationTimingType used for creating the navigation timing entry for the new Document

    void visit_edges(Visitor& visitor) override;
};

}
