/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TargetSnapshotParams.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

// The serializable values captured before entering the navigate algorithm's in-parallel steps.
// The UI process uses these to create the pending history entry after the unload check.
struct NavigationStartRequest {
    CrossProcessId navigable_id;
    URL::URL url;
    DocumentResource document_resource;
    Fetch::Infrastructure::Request::ReferrerType request_referrer;
    ReferrerPolicy::ReferrerPolicy request_referrer_policy;
    URL::Origin initiator_origin;
    Optional<URL::URL> initiator_base_url;
    Utf16String navigable_target_name;
    NavigationSourceSnapshot source_snapshot_params;
    TargetSnapshotParams target_snapshot_params;
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type;
    Bindings::NavigationHistoryBehavior history_handling;
    UserNavigationInvolvement user_involvement;
    Utf16String navigation_id;
    StorageSerializationRecord classic_history_api_state;
    StorageSerializationRecord navigation_api_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
};

// The serializable inputs needed to continue the navigate algorithm in the process selected to
// populate the pending session history entry's document.
struct NavigationPopulationRequest {
    CrossProcessId navigable_id;
    PendingSessionHistoryEntryDescriptor history_entry;
    NavigationSourceSnapshot source_snapshot_params;
    TargetSnapshotParams target_snapshot_params;
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type;
    Bindings::NavigationHistoryBehavior history_handling;
    UserNavigationInvolvement user_involvement;
    Utf16String navigation_id;
};

// The result of running population through step 4. The UI process applies the redirect
// mutations to its pending entry before selecting the process that will run step 5.
struct NavigationPopulationResult {
    NavigationParamsVariantDescriptor navigation_params;
    Optional<URL::URL> redirected_url;
    Optional<StorageSerializationRecord> classic_history_api_state;
    Optional<SessionHistoryDocumentStateDescriptor> replacement_document_state;
    bool resource_cleared { false };
};

struct HistoryNavigationPopulation {
    NavigationPopulationRequest request;
    NavigationPopulationResult result;
};

WEB_API NavigationPopulationRequest create_navigation_population_request(NavigationStartRequest, CrossProcessId document_state_id);
WEB_API void apply_navigation_population_result(NavigationPopulationRequest&, NavigationPopulationResult const&);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationStartRequest const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationStartRequest> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationPopulationRequest const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationPopulationRequest> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationPopulationResult const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationPopulationResult> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::HistoryNavigationPopulation const&);

template<>
WEB_API ErrorOr<Web::HTML::HistoryNavigationPopulation> decode(Decoder&);

}
