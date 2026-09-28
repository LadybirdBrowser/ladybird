/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

static SerializedPolicyContainer serialize_request_policy_container(Fetch::Infrastructure::Request const& request)
{
    return request.policy_container().visit(
        [&request](Fetch::Infrastructure::Request::PolicyContainer) {
            VERIFY(request.client());
            return request.client()->policy_container()->serialize();
        },
        [](GC::Ref<PolicyContainer> policy_container) {
            return policy_container->serialize();
        });
}

static NavigationRequestDescriptor create_navigation_request_descriptor(Fetch::Infrastructure::Request const& request)
{
    return {
        .url_list = request.url_list(),
        .method = request.method(),
        .client_is_null = !request.client(),
        .referrer = request.referrer(),
        .referrer_policy = request.referrer_policy(),
        .policy_container = serialize_request_policy_container(request),
        .redirect_count = request.redirect_count(),
    };
}

static Optional<ByteBuffer> copy_body_source(Fetch::Infrastructure::Body const& body)
{
    return body.source().visit(
        [](Empty) -> Optional<ByteBuffer> { return {}; },
        [](ByteBuffer const& bytes) -> Optional<ByteBuffer> { return MUST(ByteBuffer::copy(bytes)); },
        [](Core::ImmutableBytes const& bytes) -> Optional<ByteBuffer> { return MUST(ByteBuffer::copy(bytes.bytes())); },
        [](GC::Ref<FileAPI::Blob> const& blob) -> Optional<ByteBuffer> { return MUST(ByteBuffer::copy(blob->raw_bytes())); });
}

static NavigationResponseDescriptor create_navigation_response_descriptor(Fetch::Infrastructure::Response const& response)
{
    Vector<HTTP::Header> headers;
    headers.ensure_capacity(response.header_list()->headers().size());
    for (auto const& header : *response.header_list())
        headers.unchecked_append(header);

    NavigationResponseBody body;
    if (auto const& request = response.request_server_request(); request.has_value()) {
        body = NavigationResponseBodyHandle {
            .request_server_client_id = request->client_id,
            .request_server_request_id = request->request_id,
        };
    }

    if (body.has<Empty>() && response.body()) {
        if (auto body_bytes = copy_body_source(*response.body()); body_bytes.has_value())
            body = body_bytes.release_value();
    }

    return {
        .url_list = response.url_list(),
        .status = response.status(),
        .status_message = response.status_message(),
        .headers = move(headers),
        .network_error_message = response.network_error_message(),
        .timing_allow_passed = response.timing_allow_passed(),
        .navigation_timing_allow_values_list = response.navigation_timing_allow_values_list(),
        .redirect_taint = response.redirect_taint(),
        .body = move(body),
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
    if (params.fetch_controller && params.fetch_controller->timing_info())
        fetch_timing_info = create_navigation_fetch_timing_info_descriptor(*params.fetch_controller->timing_info());
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
        .response = create_navigation_response_descriptor(*params.response),
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

static GC::Ptr<Fetch::Infrastructure::Request> create_navigation_request_from_descriptor(JS::Realm& realm, LocalNavigable& navigable, Optional<NavigationRequestDescriptor> const& descriptor)
{
    if (!descriptor.has_value())
        return nullptr;

    auto request = Fetch::Infrastructure::Request::create(realm.vm());
    request->set_url_list(descriptor->url_list);
    request->set_method(descriptor->method);
    if (!descriptor->client_is_null)
        request->set_client(&navigable.active_document()->relevant_settings_object());
    request->set_referrer(descriptor->referrer);
    request->set_referrer_policy(descriptor->referrer_policy);
    request->set_policy_container(create_a_policy_container_from_serialized_policy_container(descriptor->policy_container));
    request->set_redirect_count(descriptor->redirect_count);
    return request;
}

static GC::Ptr<Fetch::Infrastructure::Body> adopt_navigation_response_body(JS::Realm& realm, NavigationResponseBodyHandle handle, Fetch::Infrastructure::Response& response, RefPtr<Fetch::Infrastructure::FetchTimingInfo> timing_info)
{
    if (!ResourceLoader::is_initialized() || !ResourceLoader::the().request_client())
        return {};

    // Preserve the original RequestServer identity while WebContent decides whether this response will become a
    // document or a download. A download is adopted by the UI process using that identity.
    auto request = ResourceLoader::the().request_client()->adopt_request(
        handle.request_server_client_id,
        handle.request_server_request_id,
        Requests::RequestClient::TransferLease::Yes);
    if (!request)
        return {};

    // Navigation may hand this response back to the UI process as a download. Do not consume any of its body before
    // populate_session_history_entry_document() decides whether to load it as a document or transfer it again.
    request->set_body_delivery_paused(true);

    auto stream = realm.heap().allocate<Streams::ReadableStream>();
    auto pull_algorithm = GC::create_function(realm.heap(), [&realm]() {
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });
    auto cancel_algorithm = GC::create_function(realm.heap(), [&realm, request](JS::Value) {
        request->stop();
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });
    stream->set_up_with_byte_reading_support(realm, pull_algorithm, cancel_algorithm);

    auto body = Fetch::Infrastructure::Body::create(stream);
    auto receiver = realm.heap().allocate<Fetch::Fetching::FetchedDataReceiver>(stream);
    receiver->set_response(response);
    receiver->set_body(body);
    auto receiver_root = GC::make_root(receiver);
    auto cross_origin_isolated_capability = Bindings::principal_host_defined_environment_settings_object(realm).cross_origin_isolated_capability();

    request->set_unbuffered_request_callbacks(
        [](NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CameFromCache) {},
        [receiver_root, &realm](Requests::ResponseData data) {
            receiver_root->handle_network_data(realm, move(data), Fetch::Fetching::FetchedDataReceiver::NetworkState::Ongoing);
        },
        [receiver_root](Core::ImmutableBytes data) {
            receiver_root->set_cached_response_body(move(data));
        },
        [receiver_root, stream = GC::make_root(stream), body = GC::make_root(body), timing_info, cross_origin_isolated_capability, &realm](u64, Requests::RequestTimingInfo const& request_timing_info, Optional<Requests::NetworkError> network_error) {
            TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
            if (!network_error.has_value()) {
                // AD-HOC: Nothing reports timing for a navigation fetch, so record the network phases and end time here
                //         for the Document's navigation timing entry.
                if (timing_info) {
                    timing_info->update_final_timings(request_timing_info, cross_origin_isolated_capability);
                    timing_info->set_end_time(HighResolutionTime::unsafe_shared_current_time());
                }
                receiver_root->handle_network_data(realm, Requests::ResponseData::from_bytes({}), Fetch::Fetching::FetchedDataReceiver::NetworkState::Complete);
                return;
            }

            body->set_sniff_bytes_complete();
            if (stream->is_readable())
                stream->error(JS::TypeError::create(realm, "Transferred navigation response failed"_utf16));
        });

    response.set_request_server_request({
        .client_id = handle.request_server_client_id,
        .request_id = handle.request_server_request_id,
        .request = request,
    });
    return body;
}

static ErrorOr<GC::Ref<Fetch::Infrastructure::Response>> create_navigation_response_from_descriptor(JS::Realm& realm, NavigationResponseDescriptor descriptor, RefPtr<Fetch::Infrastructure::FetchTimingInfo> timing_info)
{
    auto response = descriptor.network_error_message.has_value()
        ? Fetch::Infrastructure::Response::network_error(realm.vm(), move(*descriptor.network_error_message))
        : Fetch::Infrastructure::Response::create(realm.vm());
    response->set_url_list(move(descriptor.url_list));
    response->set_status(descriptor.status);
    response->set_status_message(move(descriptor.status_message));
    response->set_header_list(HTTP::HeaderList::create(move(descriptor.headers)));
    response->set_timing_allow_passed(descriptor.timing_allow_passed);
    response->set_navigation_timing_allow_values_list(move(descriptor.navigation_timing_allow_values_list));
    response->set_redirect_taint(descriptor.redirect_taint);

    if (descriptor.body.has<ByteBuffer>()) {
        response->set_body(Fetch::Infrastructure::byte_sequence_as_body(realm, descriptor.body.get<ByteBuffer>().bytes()));
    } else if (descriptor.body.has<NavigationResponseBodyHandle>()) {
        auto body = adopt_navigation_response_body(realm, descriptor.body.get<NavigationResponseBodyHandle>(), response, timing_info);
        if (!body)
            return Error::from_string_literal("Unable to adopt transferred navigation response body");
        response->set_body(body);
    }
    return response;
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
    auto request = create_navigation_request_from_descriptor(realm, navigable, params.request);
    RefPtr<Fetch::Infrastructure::FetchTimingInfo> fetch_timing_info;
    if (params.fetch_timing_info.has_value())
        fetch_timing_info = create_navigation_fetch_timing_info_from_descriptor(*params.fetch_timing_info);
    auto response = TRY(create_navigation_response_from_descriptor(realm, move(params.response), fetch_timing_info));
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
    return navigation_params;
}

}
