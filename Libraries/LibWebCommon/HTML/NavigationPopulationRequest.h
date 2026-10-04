/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Bindings/Navigation.h>
#include <LibWebCommon/ContentSecurityPolicy/Directives/NavigationType.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/HTML/TargetSnapshotParams.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

// The serializable values captured before entering the navigate algorithm's in-parallel steps.
// The UI process uses these to create the pending history entry after the unload check.
struct NavigationStartRequest {
    CrossProcessId navigable_id;
    URL::URL url;
    DocumentResource document_resource;
    Fetch::Infrastructure::RequestReferrerType request_referrer;
    ReferrerPolicy::ReferrerPolicy request_referrer_policy;
    URL::Origin initiator_origin;
    Optional<URL::URL> initiator_base_url;
    Utf16String navigable_target_name;
    NavigationSourceSnapshot source_snapshot_params;
    TargetSnapshotParams target_snapshot_params;
    ContentSecurityPolicy::Directives::NavigationType csp_navigation_type;
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
    ContentSecurityPolicy::Directives::NavigationType csp_navigation_type;
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
    // The origin the UI process gives a document created for inline content in place of the response.
    Optional<URL::Origin> inline_content_origin {};
    Optional<CrossProcessId> document_id {};
};

struct HistoryNavigationPopulation {
    NavigationPopulationRequest request;
    NavigationPopulationResult result;
};

WEBCOMMON_API NavigationPopulationRequest create_navigation_population_request(NavigationStartRequest, CrossProcessId document_state_id);
WEBCOMMON_API void apply_navigation_population_result(NavigationPopulationRequest&, NavigationPopulationResult const&);

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationStartRequest const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::NavigationStartRequest> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationPopulationRequest const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::NavigationPopulationRequest> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationPopulationResult const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::NavigationPopulationResult> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::HistoryNavigationPopulation const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::HistoryNavigationPopulation> decode(Decoder&);

}
