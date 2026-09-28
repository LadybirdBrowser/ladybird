/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::OpenerPolicy const& policy)
{
    TRY(encoder.encode(policy.value));
    TRY(encoder.encode(policy.reporting_endpoint));
    TRY(encoder.encode(policy.report_only_value));
    TRY(encoder.encode(policy.report_only_reporting_endpoint));
    return {};
}

template<>
ErrorOr<Web::HTML::OpenerPolicy> decode(Decoder& decoder)
{
    return Web::HTML::OpenerPolicy {
        .value = TRY(decoder.decode<Web::HTML::OpenerPolicyValue>()),
        .reporting_endpoint = TRY(decoder.decode<Optional<Utf16String>>()),
        .report_only_value = TRY(decoder.decode<Web::HTML::OpenerPolicyValue>()),
        .report_only_reporting_endpoint = TRY(decoder.decode<Optional<Utf16String>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::OpenerPolicyEnforcementResult const& result)
{
    TRY(encoder.encode(result.needs_a_browsing_context_group_switch));
    TRY(encoder.encode(result.would_need_a_browsing_context_group_switch_due_to_report_only));
    TRY(encoder.encode(result.url));
    TRY(encoder.encode(result.origin));
    TRY(encoder.encode(result.opener_policy));
    TRY(encoder.encode(result.current_context_is_navigation_source));
    return {};
}

template<>
ErrorOr<Web::HTML::OpenerPolicyEnforcementResult> decode(Decoder& decoder)
{
    return Web::HTML::OpenerPolicyEnforcementResult {
        .needs_a_browsing_context_group_switch = TRY(decoder.decode<bool>()),
        .would_need_a_browsing_context_group_switch_due_to_report_only = TRY(decoder.decode<bool>()),
        .url = TRY(decoder.decode<URL::URL>()),
        .origin = TRY(decoder.decode<URL::Origin>()),
        .opener_policy = TRY(decoder.decode<Web::HTML::OpenerPolicy>()),
        .current_context_is_navigation_source = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationRequestDescriptor const& request)
{
    TRY(encoder.encode(request.url_list));
    TRY(encoder.encode(request.method));
    TRY(encoder.encode(request.client_is_null));
    TRY(encoder.encode(request.referrer));
    TRY(encoder.encode(request.referrer_policy));
    TRY(encoder.encode(request.policy_container));
    TRY(encoder.encode(request.redirect_count));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationRequestDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NavigationRequestDescriptor {
        .url_list = TRY(decoder.decode<Vector<URL::URL>>()),
        .method = TRY(decoder.decode<ByteString>()),
        .client_is_null = TRY(decoder.decode<bool>()),
        .referrer = TRY(decoder.decode<Web::Fetch::Infrastructure::RequestReferrerType>()),
        .referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        .policy_container = TRY(decoder.decode<Web::HTML::SerializedPolicyContainer>()),
        .redirect_count = TRY(decoder.decode<u8>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationResponseBodyHandle const& handle)
{
    TRY(encoder.encode(handle.request_server_client_id));
    TRY(encoder.encode(handle.request_server_request_id));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationResponseBodyHandle> decode(Decoder& decoder)
{
    return Web::HTML::NavigationResponseBodyHandle {
        .request_server_client_id = TRY(decoder.decode<int>()),
        .request_server_request_id = TRY(decoder.decode<u64>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationResponseDescriptor const& response)
{
    TRY(encoder.encode(response.url_list));
    TRY(encoder.encode(response.status));
    TRY(encoder.encode(response.status_message));
    TRY(encoder.encode(response.headers));
    TRY(encoder.encode(response.network_error_message));
    TRY(encoder.encode(response.timing_allow_passed));
    TRY(encoder.encode(response.cache_state));
    TRY(encoder.encode(response.navigation_timing_allow_values_list));
    TRY(encoder.encode(response.redirect_taint));
    TRY(encoder.encode(response.body));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationResponseDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NavigationResponseDescriptor {
        .url_list = TRY(decoder.decode<Vector<URL::URL>>()),
        .status = TRY(decoder.decode<u16>()),
        .status_message = TRY(decoder.decode<ByteString>()),
        .headers = TRY(decoder.decode<Vector<HTTP::Header>>()),
        .network_error_message = TRY(decoder.decode<Optional<String>>()),
        .timing_allow_passed = TRY(decoder.decode<bool>()),
        .cache_state = TRY(decoder.decode<Optional<Web::Fetch::Infrastructure::ResponseCacheState>>()),
        .navigation_timing_allow_values_list = TRY(decoder.decode<Vector<Vector<String>>>()),
        .redirect_taint = TRY(decoder.decode<Web::Fetch::Infrastructure::RedirectTaint>()),
        .body = TRY(decoder.decode<Web::HTML::NavigationResponseBody>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationEnvironmentDescriptor const& environment)
{
    TRY(encoder.encode(environment.id));
    TRY(encoder.encode(environment.creation_url));
    TRY(encoder.encode(environment.top_level_creation_url));
    TRY(encoder.encode(environment.top_level_origin));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationEnvironmentDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NavigationEnvironmentDescriptor {
        .id = TRY(decoder.decode<Web::HTML::EnvironmentId>()),
        .creation_url = TRY(decoder.decode<URL::URL>()),
        .top_level_creation_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .top_level_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Fetch::Infrastructure::ConnectionTimingInfo const& timing_info)
{
    TRY(encoder.encode(timing_info.domain_lookup_start_time));
    TRY(encoder.encode(timing_info.domain_lookup_end_time));
    TRY(encoder.encode(timing_info.connection_start_time));
    TRY(encoder.encode(timing_info.connection_end_time));
    TRY(encoder.encode(timing_info.secure_connection_start_time));
    TRY(encoder.encode(timing_info.alpn_negotiated_protocol));
    return {};
}

template<>
ErrorOr<Web::Fetch::Infrastructure::ConnectionTimingInfo> decode(Decoder& decoder)
{
    return Web::Fetch::Infrastructure::ConnectionTimingInfo {
        .domain_lookup_start_time = TRY(decoder.decode<double>()),
        .domain_lookup_end_time = TRY(decoder.decode<double>()),
        .connection_start_time = TRY(decoder.decode<double>()),
        .connection_end_time = TRY(decoder.decode<double>()),
        .secure_connection_start_time = TRY(decoder.decode<double>()),
        .alpn_negotiated_protocol = TRY(decoder.decode<ByteString>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationFetchTimingInfoDescriptor const& timing_info)
{
    TRY(encoder.encode(timing_info.start_time));
    TRY(encoder.encode(timing_info.redirect_start_time));
    TRY(encoder.encode(timing_info.redirect_end_time));
    TRY(encoder.encode(timing_info.post_redirect_start_time));
    TRY(encoder.encode(timing_info.final_service_worker_start_time));
    TRY(encoder.encode(timing_info.final_network_request_start_time));
    TRY(encoder.encode(timing_info.first_interim_network_response_start_time));
    TRY(encoder.encode(timing_info.final_network_response_start_time));
    TRY(encoder.encode(timing_info.end_time));
    TRY(encoder.encode(timing_info.final_connection_timing_info));
    TRY(encoder.encode(timing_info.server_timing_headers));
    TRY(encoder.encode(timing_info.render_blocking));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationFetchTimingInfoDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NavigationFetchTimingInfoDescriptor {
        .start_time = TRY(decoder.decode<double>()),
        .redirect_start_time = TRY(decoder.decode<double>()),
        .redirect_end_time = TRY(decoder.decode<double>()),
        .post_redirect_start_time = TRY(decoder.decode<double>()),
        .final_service_worker_start_time = TRY(decoder.decode<double>()),
        .final_network_request_start_time = TRY(decoder.decode<double>()),
        .first_interim_network_response_start_time = TRY(decoder.decode<double>()),
        .final_network_response_start_time = TRY(decoder.decode<double>()),
        .end_time = TRY(decoder.decode<double>()),
        .final_connection_timing_info = TRY(decoder.decode<Optional<Web::Fetch::Infrastructure::ConnectionTimingInfo>>()),
        .server_timing_headers = TRY(decoder.decode<Vector<String>>()),
        .render_blocking = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NonFetchSchemeNavigationParamsDescriptor const& params)
{
    TRY(encoder.encode(params.id));
    TRY(encoder.encode(params.navigable_id));
    TRY(encoder.encode(params.url));
    TRY(encoder.encode(params.target_snapshot_sandboxing_flags));
    TRY(encoder.encode(params.source_snapshot_has_transient_activation));
    TRY(encoder.encode(params.initiator_origin));
    TRY(encoder.encode(params.navigation_timing_type));
    TRY(encoder.encode(params.user_involvement));
    return {};
}

template<>
ErrorOr<Web::HTML::NonFetchSchemeNavigationParamsDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NonFetchSchemeNavigationParamsDescriptor {
        .id = TRY(decoder.decode<Optional<Utf16String>>()),
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .url = TRY(decoder.decode<URL::URL>()),
        .target_snapshot_sandboxing_flags = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .source_snapshot_has_transient_activation = TRY(decoder.decode<bool>()),
        .initiator_origin = TRY(decoder.decode<URL::Origin>()),
        .navigation_timing_type = TRY(decoder.decode<Web::Bindings::NavigationTimingType>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigationParamsDescriptor const& params)
{
    TRY(encoder.encode(params.id));
    TRY(encoder.encode(params.navigable_id));
    TRY(encoder.encode(params.request));
    TRY(encoder.encode(params.response));
    TRY(encoder.encode(params.fetch_timing_info));
    TRY(encoder.encode(params.coop_enforcement_result));
    TRY(encoder.encode(params.reserved_environment));
    TRY(encoder.encode(params.origin));
    TRY(encoder.encode(params.policy_container));
    TRY(encoder.encode(params.final_sandboxing_flag_set));
    TRY(encoder.encode(params.iframe_element_referrer_policy));
    TRY(encoder.encode(params.opener_policy));
    TRY(encoder.encode(params.navigation_timing_type));
    TRY(encoder.encode(params.about_base_url));
    TRY(encoder.encode(params.user_involvement));
    TRY(encoder.encode(params.agent_cluster_id));
    TRY(encoder.encode(params.new_browsing_context_group_id));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationParamsDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NavigationParamsDescriptor {
        .id = TRY(decoder.decode<Optional<Utf16String>>()),
        .navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .request = TRY(decoder.decode<Optional<Web::HTML::NavigationRequestDescriptor>>()),
        .response = TRY(decoder.decode<Web::HTML::NavigationResponseDescriptor>()),
        .fetch_timing_info = TRY(decoder.decode<Optional<Web::HTML::NavigationFetchTimingInfoDescriptor>>()),
        .coop_enforcement_result = TRY(decoder.decode<Web::HTML::OpenerPolicyEnforcementResult>()),
        .reserved_environment = TRY(decoder.decode<Optional<Web::HTML::NavigationEnvironmentDescriptor>>()),
        .origin = TRY(decoder.decode<URL::Origin>()),
        .policy_container = TRY(decoder.decode<Web::HTML::SerializedPolicyContainer>()),
        .final_sandboxing_flag_set = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .iframe_element_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        .opener_policy = TRY(decoder.decode<Web::HTML::OpenerPolicy>()),
        .navigation_timing_type = TRY(decoder.decode<Web::Bindings::NavigationTimingType>()),
        .about_base_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
        .agent_cluster_id = TRY(decoder.decode<Optional<u64>>()),
        .new_browsing_context_group_id = TRY(decoder.decode<Optional<u64>>()),
    };
}

}
