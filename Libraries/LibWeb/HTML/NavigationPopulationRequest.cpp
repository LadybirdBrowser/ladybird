/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>

namespace Web::HTML {

NavigationPopulationRequest create_navigation_population_request(NavigationStartRequest start_request, CrossProcessId document_state_id)
{
    // 4. Let documentState be a new document state with
    //    request referrer policy: referrerPolicy
    //    initiator origin: initiatorOriginSnapshot
    //    resource: documentResource
    //    navigable target name: navigable's target name
    SessionHistoryDocumentStateDescriptor document_state {
        .id = document_state_id,
        .history_policy_container = DocumentState::Client::Tag,
        .request_referrer = move(start_request.request_referrer),
        .request_referrer_policy = start_request.request_referrer_policy,
        .initiator_origin = start_request.initiator_origin,
        .origin = {},
        .about_base_url = {},
        .resource = move(start_request.document_resource),
        .reload_pending = false,
        .ever_populated = false,
        .navigable_target_name = move(start_request.navigable_target_name),
        .nested_histories = {},
    };

    // 5. If url matches about:blank or is about:srcdoc, then:
    // FIXME: Is calling url_matches_about_srcdoc() correct? https://github.com/whatwg/html/issues/10900
    if (url_matches_about_blank(start_request.url) || url_matches_about_srcdoc(start_request.url)) {
        // AD-HOC: documentResource cannot be null if url is about:srcdoc since create navigation params from a
        //         srcdoc resource expects it to contain a string.
        if (url_matches_about_srcdoc(start_request.url) && document_state.resource.has<Empty>())
            document_state.resource = Utf16String {};

        // 1. Set documentState's origin to initiatorOriginSnapshot.
        document_state.origin = document_state.initiator_origin;

        // 2. Set documentState's about base URL to initiatorBaseURLSnapshot.
        document_state.about_base_url = move(start_request.initiator_base_url);
    }

    // 6. Let historyEntry be a new session history entry, with its URL set to url and its document state set to documentState.
    PendingSessionHistoryEntryDescriptor history_entry {
        .url = move(start_request.url),
        .document_state = move(document_state),
        .classic_history_api_state = move(start_request.classic_history_api_state),
        .navigation_api_state = move(start_request.navigation_api_state),
        .navigation_api_key = move(start_request.navigation_api_key),
        .navigation_api_id = move(start_request.navigation_api_id),
        .scroll_restoration_mode = ScrollRestorationMode::Auto,
        .scroll_position_data = {},
    };

    return {
        .navigable_id = start_request.navigable_id,
        .history_entry = move(history_entry),
        .source_snapshot_params = move(start_request.source_snapshot_params),
        .target_snapshot_params = start_request.target_snapshot_params,
        .csp_navigation_type = start_request.csp_navigation_type,
        .history_handling = start_request.history_handling,
        .user_involvement = start_request.user_involvement,
        .navigation_id = move(start_request.navigation_id),
    };
}

void apply_navigation_population_result(NavigationPopulationRequest& request, NavigationPopulationResult const& result)
{
    if (result.replacement_document_state.has_value())
        request.history_entry.document_state = *result.replacement_document_state;
    if (result.redirected_url.has_value())
        request.history_entry.url = *result.redirected_url;
    if (result.classic_history_api_state.has_value())
        request.history_entry.classic_history_api_state = *result.classic_history_api_state;
    if (result.resource_cleared)
        request.history_entry.document_state.resource = Empty {};
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationStartRequest const& request)
{
    TRY(encoder.encode(request.navigable_id));
    TRY(encoder.encode(request.url));
    TRY(encoder.encode(request.document_resource));
    TRY(encoder.encode(request.request_referrer));
    TRY(encoder.encode(request.request_referrer_policy));
    TRY(encoder.encode(request.initiator_origin));
    TRY(encoder.encode(request.initiator_base_url));
    TRY(encoder.encode(request.navigable_target_name));
    TRY(encoder.encode(request.source_snapshot_params));
    TRY(encoder.encode(request.target_snapshot_params.sandboxing_flags));
    TRY(encoder.encode(request.target_snapshot_params.iframe_element_referrer_policy));
    TRY(encoder.encode(request.csp_navigation_type));
    TRY(encoder.encode(request.history_handling));
    TRY(encoder.encode(request.user_involvement));
    TRY(encoder.encode(request.navigation_id));
    TRY(encoder.encode(request.classic_history_api_state));
    TRY(encoder.encode(request.navigation_api_state));
    TRY(encoder.encode(request.navigation_api_key));
    TRY(encoder.encode(request.navigation_api_id));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationStartRequest> decode(Decoder& decoder)
{
    return Web::HTML::NavigationStartRequest {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .url = TRY(decoder.decode<URL::URL>()),
        .document_resource = TRY(decoder.decode<Web::HTML::DocumentResource>()),
        .request_referrer = TRY(decoder.decode<Web::Fetch::Infrastructure::Request::ReferrerType>()),
        .request_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        .initiator_origin = TRY(decoder.decode<URL::Origin>()),
        .initiator_base_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .navigable_target_name = TRY(decoder.decode<Utf16String>()),
        .source_snapshot_params = TRY(decoder.decode<Web::HTML::NavigationSourceSnapshot>()),
        .target_snapshot_params = {
            .sandboxing_flags = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
            .iframe_element_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        },
        .csp_navigation_type = TRY(decoder.decode<Web::ContentSecurityPolicy::Directives::Directive::NavigationType>()),
        .history_handling = TRY(decoder.decode<Web::Bindings::NavigationHistoryBehavior>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
        .navigation_id = TRY(decoder.decode<Utf16String>()),
        .classic_history_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>()),
        .navigation_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>()),
        .navigation_api_key = TRY(decoder.decode<Utf16String>()),
        .navigation_api_id = TRY(decoder.decode<Utf16String>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationPopulationRequest const& request)
{
    TRY(encoder.encode(request.navigable_id));
    TRY(encoder.encode(request.history_entry));
    TRY(encoder.encode(request.source_snapshot_params));
    TRY(encoder.encode(request.target_snapshot_params.sandboxing_flags));
    TRY(encoder.encode(request.target_snapshot_params.iframe_element_referrer_policy));
    TRY(encoder.encode(request.csp_navigation_type));
    TRY(encoder.encode(request.history_handling));
    TRY(encoder.encode(request.user_involvement));
    TRY(encoder.encode(request.navigation_id));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationPopulationRequest> decode(Decoder& decoder)
{
    return Web::HTML::NavigationPopulationRequest {
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .history_entry = TRY(decoder.decode<Web::HTML::PendingSessionHistoryEntryDescriptor>()),
        .source_snapshot_params = TRY(decoder.decode<Web::HTML::NavigationSourceSnapshot>()),
        .target_snapshot_params = {
            .sandboxing_flags = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
            .iframe_element_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        },
        .csp_navigation_type = TRY(decoder.decode<Web::ContentSecurityPolicy::Directives::Directive::NavigationType>()),
        .history_handling = TRY(decoder.decode<Web::Bindings::NavigationHistoryBehavior>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
        .navigation_id = TRY(decoder.decode<Utf16String>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationPopulationResult const& result)
{
    TRY(encoder.encode(result.navigation_params));
    TRY(encoder.encode(result.redirected_url));
    TRY(encoder.encode(result.classic_history_api_state));
    TRY(encoder.encode(result.replacement_document_state));
    TRY(encoder.encode(result.resource_cleared));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationPopulationResult> decode(Decoder& decoder)
{
    return Web::HTML::NavigationPopulationResult {
        .navigation_params = TRY(decoder.decode<Web::HTML::NavigationParamsVariantDescriptor>()),
        .redirected_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .classic_history_api_state = TRY(decoder.decode<Optional<Web::HTML::StorageSerializationRecord>>()),
        .replacement_document_state = TRY(decoder.decode<Optional<Web::HTML::SessionHistoryDocumentStateDescriptor>>()),
        .resource_cleared = TRY(decoder.decode<bool>()),
    };
}

}
