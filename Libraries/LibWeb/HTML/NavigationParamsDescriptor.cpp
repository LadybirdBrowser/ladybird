/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigationParamsDescriptor.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
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
        .referrer = request.referrer(),
        .referrer_policy = request.referrer_policy(),
        .policy_container = serialize_request_policy_container(request),
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
        .body = move(body),
    };
}

static NavigationParamsDescriptor create_navigation_params_descriptor(NavigationParams const& params)
{
    Optional<NavigationRequestDescriptor> request;
    if (params.request)
        request = create_navigation_request_descriptor(*params.request);

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
        .coop_enforcement_result = params.coop_enforcement_result,
        .reserved_environment = move(reserved_environment),
        .origin = params.origin,
        .policy_container = params.policy_container->serialize(),
        .final_sandboxing_flag_set = params.final_sandboxing_flag_set,
        .iframe_element_referrer_policy = params.iframe_element_referrer_policy,
        .opener_policy = params.opener_policy,
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
    request->set_client(&navigable.active_document()->relevant_settings_object());
    request->set_referrer(descriptor->referrer);
    request->set_referrer_policy(descriptor->referrer_policy);
    request->set_policy_container(create_a_policy_container_from_serialized_policy_container(descriptor->policy_container));
    return request;
}

static GC::Ptr<Fetch::Infrastructure::Body> adopt_navigation_response_body(JS::Realm& realm, NavigationResponseBodyHandle handle, Fetch::Infrastructure::Response& response)
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

    request->set_unbuffered_request_callbacks(
        [](NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String> const&, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CameFromCache) {},
        [receiver_root, &realm](Requests::ResponseData data) {
            receiver_root->handle_network_data(realm, move(data), Fetch::Fetching::FetchedDataReceiver::NetworkState::Ongoing);
        },
        [receiver_root](Core::ImmutableBytes data) {
            receiver_root->set_cached_response_body(move(data));
        },
        [receiver_root, stream = GC::make_root(stream), body = GC::make_root(body), &realm](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
            if (!network_error.has_value()) {
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

static ErrorOr<GC::Ref<Fetch::Infrastructure::Response>> create_navigation_response_from_descriptor(JS::Realm& realm, NavigationResponseDescriptor descriptor)
{
    auto response = descriptor.network_error_message.has_value()
        ? Fetch::Infrastructure::Response::network_error(realm.vm(), move(*descriptor.network_error_message))
        : Fetch::Infrastructure::Response::create(realm.vm());
    response->set_url_list(move(descriptor.url_list));
    response->set_status(descriptor.status);
    response->set_status_message(move(descriptor.status_message));
    response->set_header_list(HTTP::HeaderList::create(move(descriptor.headers)));
    response->set_timing_allow_passed(descriptor.timing_allow_passed);

    if (descriptor.body.has<ByteBuffer>()) {
        response->set_body(Fetch::Infrastructure::byte_sequence_as_body(realm, descriptor.body.get<ByteBuffer>().bytes()));
    } else if (descriptor.body.has<NavigationResponseBodyHandle>()) {
        auto body = adopt_navigation_response_body(realm, descriptor.body.get<NavigationResponseBodyHandle>(), response);
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
            params.user_involvement);
    }

    auto params = descriptor.get<NavigationParamsDescriptor>();
    VERIFY(params.navigable_id == navigable.id());
    auto request = create_navigation_request_from_descriptor(realm, navigable, params.request);
    auto response = TRY(create_navigation_response_from_descriptor(realm, move(params.response)));
    GC::Ptr<Environment> reserved_environment;
    if (params.reserved_environment.has_value()) {
        reserved_environment = Environment::create(
            move(params.reserved_environment->id),
            move(params.reserved_environment->creation_url),
            move(params.reserved_environment->top_level_creation_url),
            move(params.reserved_environment->top_level_origin),
            navigable.active_browsing_context());
    }

    return realm.heap().allocate<NavigationParams>(
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
        move(params.about_base_url),
        params.user_involvement);
}

}

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
    TRY(encoder.encode(request.referrer));
    TRY(encoder.encode(request.referrer_policy));
    TRY(encoder.encode(request.policy_container));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigationRequestDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::NavigationRequestDescriptor {
        .url_list = TRY(decoder.decode<Vector<URL::URL>>()),
        .method = TRY(decoder.decode<ByteString>()),
        .referrer = TRY(decoder.decode<Web::Fetch::Infrastructure::Request::ReferrerType>()),
        .referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        .policy_container = TRY(decoder.decode<Web::HTML::SerializedPolicyContainer>()),
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
        .id = TRY(decoder.decode<Utf16String>()),
        .creation_url = TRY(decoder.decode<URL::URL>()),
        .top_level_creation_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .top_level_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
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
    TRY(encoder.encode(params.coop_enforcement_result));
    TRY(encoder.encode(params.reserved_environment));
    TRY(encoder.encode(params.origin));
    TRY(encoder.encode(params.policy_container));
    TRY(encoder.encode(params.final_sandboxing_flag_set));
    TRY(encoder.encode(params.iframe_element_referrer_policy));
    TRY(encoder.encode(params.opener_policy));
    TRY(encoder.encode(params.about_base_url));
    TRY(encoder.encode(params.user_involvement));
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
        .coop_enforcement_result = TRY(decoder.decode<Web::HTML::OpenerPolicyEnforcementResult>()),
        .reserved_environment = TRY(decoder.decode<Optional<Web::HTML::NavigationEnvironmentDescriptor>>()),
        .origin = TRY(decoder.decode<URL::Origin>()),
        .policy_container = TRY(decoder.decode<Web::HTML::SerializedPolicyContainer>()),
        .final_sandboxing_flag_set = TRY(decoder.decode<Web::HTML::SandboxingFlagSet>()),
        .iframe_element_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        .opener_policy = TRY(decoder.decode<Web::HTML::OpenerPolicy>()),
        .about_base_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
    };
}

}
