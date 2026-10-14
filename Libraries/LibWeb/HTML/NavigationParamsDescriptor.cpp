/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebDriver/BiDi/Network.h>

namespace Web::HTML {

static SerializedPolicyContainer serialize_request_policy_container(Fetch::Infrastructure::Request const& request)
{
    return request.policy_container().visit(
        [&request](Fetch::Infrastructure::Request::PolicyContainer) {
            auto client = Fetch::Fetching::resolve_fetch_client(request);
            VERIFY(client.has_value());
            return client->visit(
                [](GC::Ref<EnvironmentSettingsObject> const& settings_object) { return settings_object->policy_container()->serialize(); },
                [](NonnullRefPtr<RemoteEnvironmentSettings const> const& settings) { return settings->settings.policy_container; });
        },
        [](NonnullRefPtr<PolicyContainer const> const& policy_container) {
            return policy_container->serialize();
        });
}

static NavigationRequestDescriptor create_navigation_request_descriptor(Fetch::Infrastructure::Request const& request)
{
    return {
        .url_list = request.url_list(),
        .method = request.method(),
        .client_is_null = !request.client().has_value(),
        .referrer = request.referrer(),
        .referrer_policy = request.referrer_policy(),
        .policy_container = serialize_request_policy_container(request),
        .redirect_count = request.redirect_count(),
        .text_directive_user_activation = request.text_directive_user_activation(),
        .browsing_context_group_has_multiple_contexts = request.browsing_context_group_has_multiple_contexts(),
        .cache_mode = request.cache_mode(),
    };
}

static NavigationResponseDescriptor create_navigation_response_descriptor(Fetch::Infrastructure::ResponseView const& response, Fetch::Infrastructure::Request const* request, Fetch::Infrastructure::FetchTimingInfo const* timing_info)
{
    Vector<HTTP::Header> headers;
    headers.ensure_capacity(response.header_list()->headers().size());
    for (auto const& header : *response.header_list())
        headers.unchecked_append(header);

    NavigationResponseBody body;
    if (auto const& network_transport = response.network_transport(); network_transport.has_value()) {
        auto const& lease = network_transport->handle->transfer_lease();
        VERIFY(lease.has_value());
        body = NavigationResponseBodyHandle {
            .request_server_client_id = lease->source_client_id,
            .request_server_request_id = lease->source_request_id,
            .request_server_top_level_site = {},
        };
    }

    if (body.has<Empty>()) {
        if (auto source = response.body_source(); source.has_value())
            body = MUST(source->copy_to_byte_buffer());
    }

    auto network_response = request && timing_info && response.network_transport().has_value() ? WebDriver::BiDi::network_response_for_navigation(*request, *timing_info, *response.record()) : Optional<JsonObject> {};
    return {
        .url_list = response.url_list(),
        .status = response.status(),
        .status_message = response.status_message(),
        .headers = move(headers),
        .network_error_message = response.network_error_message(),
        .timing_allow_passed = response.timing_allow_passed(),
        .cache_state = response.cache_state(),
        .navigation_timing_allow_values_list = response.navigation_timing_allow_values_list(),
        .redirect_taint = response.redirect_taint(),
        .body = move(body),
        .webdriver_bidi_network_response = network_response.has_value() ? JsonValue(network_response.release_value()) : JsonValue {},
    };
}

static NavigationFetchTimingInfoDescriptor create_navigation_fetch_timing_info_descriptor(Fetch::Infrastructure::FetchTimingInfo const& timing_info)
{
    return {
        .start_time = timing_info.start_time(),
        .redirect_start_time = timing_info.redirect_start_time(),
        .redirect_end_time = timing_info.redirect_end_time(),
        .post_redirect_start_time = timing_info.post_redirect_start_time(),
        .final_service_worker_start_time = timing_info.final_service_worker_start_time(),
        .final_network_request_start_time = timing_info.final_network_request_start_time(),
        .first_interim_network_response_start_time = timing_info.first_interim_network_response_start_time(),
        .final_network_response_start_time = timing_info.final_network_response_start_time(),
        .end_time = timing_info.end_time(),
        .final_connection_timing_info = timing_info.final_connection_timing_info(),
        .server_timing_headers = timing_info.server_timing_headers(),
        .render_blocking = timing_info.render_blocking(),
    };
}

static NonnullRefPtr<Fetch::Infrastructure::FetchTimingInfo> create_navigation_fetch_timing_info_from_descriptor(NavigationFetchTimingInfoDescriptor const& descriptor)
{
    auto timing_info = Fetch::Infrastructure::FetchTimingInfo::create();
    timing_info->set_start_time(descriptor.start_time);
    timing_info->set_redirect_start_time(descriptor.redirect_start_time);
    timing_info->set_redirect_end_time(descriptor.redirect_end_time);
    timing_info->set_post_redirect_start_time(descriptor.post_redirect_start_time);
    timing_info->set_final_service_worker_start_time(descriptor.final_service_worker_start_time);
    timing_info->set_final_network_request_start_time(descriptor.final_network_request_start_time);
    timing_info->set_first_interim_network_response_start_time(descriptor.first_interim_network_response_start_time);
    timing_info->set_final_network_response_start_time(descriptor.final_network_response_start_time);
    timing_info->set_end_time(descriptor.end_time);
    if (descriptor.final_connection_timing_info.has_value())
        timing_info->set_final_connection_timing_info(*descriptor.final_connection_timing_info);
    timing_info->set_server_timing_headers(descriptor.server_timing_headers);
    timing_info->set_render_blocking(descriptor.render_blocking);
    return timing_info;
}

static NavigationParamsDescriptor create_navigation_params_descriptor(NavigationParams const& params)
{
    Optional<NavigationRequestDescriptor> request;
    if (params.request)
        request = create_navigation_request_descriptor(*params.request);

    Optional<NavigationFetchTimingInfoDescriptor> fetch_timing_info;
    if (params.fetch_controller && params.fetch_controller->full_timing_info())
        fetch_timing_info = create_navigation_fetch_timing_info_descriptor(*params.fetch_controller->full_timing_info());
    else if (params.fetch_timing_info)
        fetch_timing_info = create_navigation_fetch_timing_info_descriptor(*params.fetch_timing_info);

    Optional<NavigationEnvironmentDescriptor> reserved_environment;
    if (params.reserved_environment) {
        reserved_environment = NavigationEnvironmentDescriptor {
            .id = params.reserved_environment->id,
            .creation_url = params.reserved_environment->creation_url,
            .top_level_creation_url = params.reserved_environment->top_level_creation_url,
            .top_level_origin = params.reserved_environment->top_level_origin,
        };
    }

    VERIFY(params.navigable);
    VERIFY(params.response);
    VERIFY(params.policy_container);
    return {
        .id = params.id,
        .navigable_id = params.navigable->id(),
        .request = move(request),
        .response = create_navigation_response_descriptor(*params.response, params.request.ptr(), params.fetch_controller ? params.fetch_controller->full_timing_info().ptr() : params.fetch_timing_info.ptr()),
        .fetch_timing_info = move(fetch_timing_info),
        .coop_enforcement_result = params.coop_enforcement_result,
        .reserved_environment = move(reserved_environment),
        .origin = params.origin,
        .policy_container = params.policy_container->serialize(),
        .final_sandboxing_flag_set = params.final_sandboxing_flag_set,
        .iframe_element_referrer_policy = params.iframe_element_referrer_policy,
        .opener_policy = params.opener_policy,
        .navigation_timing_type = params.navigation_timing_type,
        .about_base_url = params.about_base_url,
        .user_involvement = params.user_involvement,
        .agent_cluster_id = params.agent_cluster_id,
        .agent_cluster_is_origin_keyed = params.agent_cluster_is_origin_keyed,
        .agent_cluster_cross_origin_isolation_mode = params.agent_cluster_cross_origin_isolation_mode,
        .new_browsing_context_group_id = params.new_browsing_context_group_id,
        .permissions_policy = params.permissions_policy,
    };
}

void create_navigation_params_descriptor(JS::Realm& realm, NavigationParamsVariant params, GC::Ref<NavigationParamsDescriptorCompletion> completion_steps)
{
    if (params.has<NavigationParamsNullOrError>()) {
        completion_steps->function()(params.get<NavigationParamsNullOrError>());
        return;
    }

    if (params.has<GC::Ref<NonFetchSchemeNavigationParams>>()) {
        auto const& non_fetch_params = params.get<GC::Ref<NonFetchSchemeNavigationParams>>();
        VERIFY(non_fetch_params->navigable);
        completion_steps->function()(NonFetchSchemeNavigationParamsDescriptor {
            .id = non_fetch_params->id,
            .navigable_id = non_fetch_params->navigable->id(),
            .url = non_fetch_params->url,
            .target_snapshot_sandboxing_flags = non_fetch_params->target_snapshot_sandboxing_flags,
            .source_snapshot_has_transient_activation = non_fetch_params->source_snapshot_has_transient_activation,
            .initiator_origin = non_fetch_params->initiator_origin,
            .navigation_timing_type = non_fetch_params->navigation_timing_type,
            .user_involvement = non_fetch_params->user_involvement,
        });
        return;
    }

    auto navigation_params = params.get<GC::Ref<NavigationParams>>();

    // A request that another client adopted has no lease to hand over.
    auto const& network_transport = navigation_params->response->network_transport();
    if (network_transport.has_value() && !network_transport->handle->transfer_lease().has_value()) {
        stop_body_delivery(navigation_params->body_delivery());
        completion_steps->function()(NavigationParamsNullOrError { "Unable to transfer response body"_utf16 });
        return;
    }

    auto descriptor = create_navigation_params_descriptor(navigation_params);
    auto body = navigation_params->response->body();
    if (!body || !descriptor.response.body.has<Empty>()) {
        completion_steps->function()(move(descriptor));
        return;
    }

    body->fully_read(
        realm,
        GC::create_function(realm.heap(), [descriptor = move(descriptor), completion_steps](ByteBuffer bytes) mutable {
            descriptor.response.body = move(bytes);
            completion_steps->function()(move(descriptor));
        }),
        GC::create_function(realm.heap(), [completion_steps](JS::Value) {
            completion_steps->function()(NavigationParamsNullOrError { "Unable to transfer response body"_utf16 });
        }),
        GC::Ref { realm.global_object() });
}

static RefPtr<Fetch::Infrastructure::Request> create_navigation_request_from_descriptor(LocalNavigable& navigable, Optional<NavigationRequestDescriptor> const& descriptor)
{
    if (!descriptor.has_value())
        return nullptr;

    auto request = Fetch::Infrastructure::Request::create();
    request->set_url_list(descriptor->url_list);
    request->set_method(descriptor->method);
    if (!descriptor->client_is_null)
        request->set_client(navigable.active_document()->relevant_settings_object().address());
    request->set_referrer(descriptor->referrer);
    request->set_referrer_policy(descriptor->referrer_policy);
    request->set_policy_container(create_a_policy_container_from_serialized_policy_container(descriptor->policy_container));
    request->set_redirect_count(descriptor->redirect_count);
    request->set_text_directive_user_activation(descriptor->text_directive_user_activation);
    request->set_browsing_context_group_has_multiple_contexts(descriptor->browsing_context_group_has_multiple_contexts);
    request->set_cache_mode(descriptor->cache_mode);
    return request;
}

// Reports the WebDriver BiDi event that ends an adopted navigation response, unless a download has taken it over.
static Function<void(Optional<StringView>)> network_navigation_response_observer(LocalNavigable& navigable, Fetch::Infrastructure::Response& response, RefPtr<Fetch::Infrastructure::FetchTimingInfo> timing_info, Optional<JsonObject> network_response, RefPtr<NavigationResponseNetworkEventState> network_response_state)
{
    auto observed_navigable = network_response.has_value() ? GC::Weak<LocalNavigable> { navigable } : GC::Weak<LocalNavigable> {};

    // NB: Observing its navigable must not keep the navigable alive.
    return [response = NonnullRefPtr { response }, observed_navigable = move(observed_navigable), timing_info = move(timing_info), network_response = move(network_response), network_response_state = move(network_response_state)](Optional<StringView> error) {
        if (!network_response.has_value() || !observed_navigable || network_response_state->owner != NavigationResponseNetworkEventState::Owner::Renderer)
            return;
        network_response_state->owner = NavigationResponseNetworkEventState::Owner::Completed;
        WebDriver::BiDi::network_navigation_response_finished(*observed_navigable, *network_response, *response, timing_info, error);
    };
}

static ErrorOr<GC::Ref<Fetch::Infrastructure::ResponseView>> create_navigation_response_from_descriptor(JS::Realm& realm, LocalNavigable& navigable, NavigationResponseDescriptor descriptor, RefPtr<Fetch::Infrastructure::FetchTimingInfo> timing_info, RefPtr<NavigationResponseNetworkEventState> network_response_state)
{
    auto response = descriptor.network_error_message.has_value()
        ? Fetch::Infrastructure::Response::network_error(move(*descriptor.network_error_message))
        : Fetch::Infrastructure::Response::create();
    response->set_url_list(move(descriptor.url_list));
    response->set_status(descriptor.status);
    response->set_status_message(move(descriptor.status_message));
    response->set_header_list(HTTP::HeaderList::create(move(descriptor.headers)));
    response->set_timing_allow_passed(descriptor.timing_allow_passed);
    response->set_cache_state(descriptor.cache_state);
    response->set_navigation_timing_allow_values_list(move(descriptor.navigation_timing_allow_values_list));
    response->set_redirect_taint(descriptor.redirect_taint);

    if (descriptor.body.has<NavigationResponseBodyHandle>()) {
        auto const& handle = descriptor.body.get<NavigationResponseBodyHandle>();
        Requests::RequestTransferLeaseKey transfer_lease { handle.request_server_client_id, handle.request_server_request_id };
        auto network_response = descriptor.webdriver_bidi_network_response.is_object() ? Optional<JsonObject>(descriptor.webdriver_bidi_network_response.as_object()) : Optional<JsonObject> {};
        auto on_network_settled = network_navigation_response_observer(navigable, response, timing_info, move(network_response), move(network_response_state));
        auto body_view = Fetch::Fetching::adopt_navigation_response_body(realm, response, handle.request_server_top_level_site, transfer_lease, timing_info, move(on_network_settled));
        if (!body_view)
            return Error::from_string_literal("Unable to adopt transferred navigation response body");
        return Fetch::Infrastructure::ResponseView::create(response, body_view);
    }

    if (descriptor.body.has<ByteBuffer>()) {
        auto bytes = Core::ImmutableBytes::adopt(move(descriptor.body.get<ByteBuffer>()));
        response->set_body(Fetch::Infrastructure::ResponseBody::from_bytes(move(bytes)));
    }
    return Fetch::Infrastructure::ResponseView::create(realm, response);
}

ErrorOr<NavigationParamsVariant> create_navigation_params_from_descriptor(JS::Realm& realm, LocalNavigable& navigable, NavigationParamsVariantDescriptor descriptor)
{
    if (descriptor.has<NavigationParamsNullOrError>())
        return descriptor.get<NavigationParamsNullOrError>();

    if (descriptor.has<NonFetchSchemeNavigationParamsDescriptor>()) {
        auto params = descriptor.get<NonFetchSchemeNavigationParamsDescriptor>();
        VERIFY(params.navigable_id == navigable.id());
        return realm.heap().allocate<NonFetchSchemeNavigationParams>(
            move(params.id),
            &navigable,
            move(params.url),
            params.target_snapshot_sandboxing_flags,
            params.source_snapshot_has_transient_activation,
            move(params.initiator_origin),
            params.navigation_timing_type,
            params.user_involvement);
    }

    auto params = descriptor.get<NavigationParamsDescriptor>();
    VERIFY(params.navigable_id == navigable.id());
    auto request = create_navigation_request_from_descriptor(navigable, params.request);
    RefPtr<Fetch::Infrastructure::FetchTimingInfo> fetch_timing_info;
    if (params.fetch_timing_info.has_value())
        fetch_timing_info = create_navigation_fetch_timing_info_from_descriptor(*params.fetch_timing_info);
    auto network_response = params.response.webdriver_bidi_network_response;
    RefPtr<NavigationResponseNetworkEventState> network_response_state;
    if (network_response.is_object())
        network_response_state = adopt_ref(*new NavigationResponseNetworkEventState);
    auto response = TRY(create_navigation_response_from_descriptor(realm, navigable, move(params.response), fetch_timing_info, network_response_state));
    GC::Ptr<Environment> reserved_environment;
    if (params.reserved_environment.has_value()) {
        reserved_environment = Environment::create(
            move(params.reserved_environment->id),
            move(params.reserved_environment->creation_url),
            move(params.reserved_environment->top_level_creation_url),
            move(params.reserved_environment->top_level_origin),
            navigable.active_browsing_context());
    }

    auto navigation_params = realm.heap().allocate<NavigationParams>(
        move(params.id),
        &navigable,
        request,
        response,
        nullptr,
        nullptr,
        move(params.coop_enforcement_result),
        reserved_environment,
        move(params.origin),
        create_a_policy_container_from_serialized_policy_container(params.policy_container),
        params.final_sandboxing_flag_set,
        params.iframe_element_referrer_policy,
        move(params.opener_policy),
        params.navigation_timing_type,
        move(params.about_base_url),
        params.user_involvement);
    navigation_params->fetch_timing_info = fetch_timing_info;
    navigation_params->webdriver_bidi_network_response = move(network_response);
    navigation_params->webdriver_bidi_network_response_state = move(network_response_state);
    navigation_params->agent_cluster_id = params.agent_cluster_id;
    navigation_params->agent_cluster_is_origin_keyed = params.agent_cluster_is_origin_keyed;
    navigation_params->agent_cluster_cross_origin_isolation_mode = params.agent_cluster_cross_origin_isolation_mode;
    navigation_params->new_browsing_context_group_id = params.new_browsing_context_group_id;
    navigation_params->permissions_policy = move(params.permissions_policy);
    return navigation_params;
}

}
