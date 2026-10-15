/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/GenericLexer.h>
#include <AK/JsonArray.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibRequests/Request.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/ClientContextSnapshot.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/Network.h>

namespace Web::WebDriver::BiDi {

using namespace Fetch::Infrastructure;
using Response = Fetch::Infrastructure::Response;

GC_DEFINE_ALLOCATOR(NetworkContinuation);

NetworkContinuation::NetworkContinuation(Callback callback)
    : m_callback(move(callback))
{
}

GC::Ref<NetworkContinuation> NetworkContinuation::create(GC::Heap& heap, Callback callback)
{
    return heap.allocate<NetworkContinuation>(move(callback));
}

void NetworkContinuation::finalize()
{
    Base::finalize();
    clear_blocked_state();
    m_callback = nullptr;
}

void NetworkContinuation::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(controller);
    visitor.visit_possible_values(m_callback.raw_capture_range());
}

void NetworkContinuation::invoke(NetworkContinuationAction action, RefPtr<Response> response)
{
    m_callback(action, response);
    m_callback = nullptr;
}

bool g_network_events_enabled { false };
bool g_network_configuration_enabled { false };

void NetworkContinuation::clear_blocked_state()
{
    request_id = {};
    phase = {};
    intercepts = {};
    fetch_params = nullptr;
    controller = nullptr;
    original_request = nullptr;
    response = nullptr;
}

static HashMap<String, GC::Root<NetworkContinuation>>& blocked_requests()
{
    static auto& requests = *new HashMap<String, GC::Root<NetworkContinuation>>;
    return requests;
}

static GC::Ptr<NetworkContinuation> blocked_request(Request const& request)
{
    auto state = request.webdriver_bidi_network_request();
    if (!state)
        return nullptr;
    auto it = blocked_requests().find(state->id);
    return it == blocked_requests().end() ? nullptr : it->value.ptr();
}

static GC::Ptr<NetworkContinuation> blocked_request(FetchController const& controller)
{
    auto fetch_params = controller.original_fetch_params();
    if (!fetch_params)
        return nullptr;
    auto blocked = blocked_request(*fetch_params->request());
    return blocked && blocked->controller.ptr() == &controller ? blocked : nullptr;
}

static void share_blocked_request_identity(FetchParams const& fetch_params)
{
    if (auto blocked = blocked_request(*fetch_params.controller()))
        fetch_params.request()->set_webdriver_bidi_network_request(blocked->fetch_params->request()->webdriver_bidi_network_request());
}

static void stop_response_transport(FetchController& controller, Response const& response)
{
    response.release_request_transfer_lease();
    controller.stop_body_transport();
}

static void resume_blocked_request(String id, NetworkContinuationAction action, RefPtr<Fetch::Infrastructure::Response> response)
{
    auto it = blocked_requests().find(id);
    if (it == blocked_requests().end())
        return;
    GC::Ptr<NetworkContinuation> continuation = it->value.ptr();
    auto controller = continuation->controller;
    auto original_response = continuation->response;
    // NB: Remove the entry before stopping transport or invoking a continuation, which can reenter Fetch.
    blocked_requests().remove(it);
    continuation->clear_blocked_state();
    if (action == NetworkContinuationAction::Fail && original_response)
        stop_response_transport(*controller, *original_response);
    continuation->invoke(action, response);
}

static void cancel_network_interception_impl(FetchParams const& fetch_params, StringView error_text)
{
    auto blocked = blocked_request(*fetch_params.controller());
    if (!blocked)
        return;
    share_blocked_request_identity(fetch_params);
    resume_blocked_request(blocked->request_id, NetworkContinuationAction::Fail, Response::network_error(String::from_utf8_with_replacement_character(error_text)));
}

void cancel_network_interception(FetchParams const& fetch_params)
{
    cancel_network_interception_impl(fetch_params, "Intercepted fetch was stopped"sv);
}

void network_session_state_changed()
{
    auto const& state = session_state();
    g_network_events_enabled = state.is_active && any_of(state.enabled_events, [](auto const& name) { return name.starts_with_bytes("network."sv); });
    g_network_configuration_enabled = state.is_active && (!state.network_default_headers.is_empty() || !state.network_user_context_headers.is_empty() || !state.network_navigable_headers.is_empty() || state.network_default_cache_behavior != "default"sv || !state.network_navigable_cache_behavior.is_empty());
    if (!state.is_active) {
        // https://w3c.github.io/webdriver-bidi/#cleanup-the-session
        while (!blocked_requests().is_empty()) {
            auto const& entry = *blocked_requests().begin();
            auto response = entry.value->response;
            resume_blocked_request(entry.key, NetworkContinuationAction::Continue, response);
        }
    }
}

static GC::Ptr<HTML::LocalNavigable> navigable_of(NetworkRequest const& state)
{
    if (!state.navigable_id.has_value())
        return {};
    return HTML::local_navigable_with_id(*state.navigable_id);
}

static GC::Ptr<HTML::Navigable> request_navigable(Request const& request)
{
    if (auto state = request.webdriver_bidi_network_request())
        return navigable_of(*state);
    if (auto const& client = request.client_snapshot(); client && client->navigable_id.has_value())
        return HTML::local_navigable_with_id(*client->navigable_id);
    if (auto const* id = request.traversable_for_user_prompts().get_pointer<HTML::CrossProcessId>())
        return HTML::local_navigable_with_id(*id);
    return nullptr;
}

static String network_context_id(HTML::Navigable& navigable)
{
    auto id = navigable.top_level_traversable()->id();
    return MUST(String::formatted("{}-{}", id.namespace_id, id.local_id));
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-cache-behavior
void apply_network_cache_behavior(Request& request)
{
    auto const& state = session_state();
    auto behavior = state.network_default_cache_behavior;
    if (auto navigable = request_navigable(request))
        behavior = state.network_navigable_cache_behavior.get(network_context_id(*navigable)).value_or(behavior);
    if (behavior == "bypass"sv)
        request.set_cache_mode(HTTP::CacheMode::NoStore);
}

// https://w3c.github.io/webdriver-bidi/#update-request-headers
static constexpr Array fetch_generated_request_header_names {
    "Content-Length"sv,
    "Accept-Encoding"sv,
    "Referer"sv,
    "Origin"sv,
    "Sec-Fetch-Dest"sv,
    "Sec-Fetch-Mode"sv,
    "Sec-Fetch-Site"sv,
    "Sec-Fetch-User"sv,
};

void apply_network_request_header_overrides(Request& request)
{
    auto state = request.webdriver_bidi_network_request();
    if (!state || !state->request_header_overrides)
        return;
    for (auto name : fetch_generated_request_header_names) {
        if (!state->request_header_overrides->contains(name))
            continue;
        request.header_list()->delete_(name);
        for (auto const& header : *state->request_header_overrides) {
            if (header.name.equals_ignoring_ascii_case(name))
                request.header_list()->append(header);
        }
    }
}

void apply_network_extra_headers(Request& request)
{
    auto const& state = session_state();
    // https://w3c.github.io/webdriver-bidi/#update-headers
    auto update_headers = [&](auto const& headers) {
        for (auto const& header : headers)
            request.header_list()->set(header);
    };
    // 2. Update headers with request and session's extra headers' default headers
    update_headers(state.network_default_headers);
    if (auto navigable = request_navigable(request)) {
        if (auto it = state.network_user_context_headers.find("default"_string); it != state.network_user_context_headers.end())
            update_headers(it->value);
        auto id = network_context_id(*navigable);
        if (auto it = state.network_navigable_headers.find(id); it != state.network_navigable_headers.end())
            update_headers(it->value);
    }
}

// https://w3c.github.io/webdriver-bidi/#get-the-network-intercepts
JsonArray get_network_intercepts(FetchParams const& fetch_params, StringView phase)
{
    JsonArray intercepts;
    if (session_state().network_intercepts.is_empty() || !is_event_enabled(MUST(String::formatted("network.{}", phase))))
        return intercepts;
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    if (state && ((phase == "beforeRequestSent"sv && state->before_request_sent.contains(request->redirect_count())) || (phase == "responseStarted"sv && state->response_started.contains(request->redirect_count()))))
        return intercepts;
    // NB: An existing request keeps its original context. A request first observed after instrumentation was
    //     enabled can still discover its context, and blocking must use that same context.
    auto navigable = request_navigable(*request);
    if (!navigable || fetch_params.is_canceled() || fetch_params.control_block().state() == FetchControlBlock::State::Stopped)
        return intercepts;
    if (!state)
        initialize_network_request(*request, navigable);
    auto context = network_context_id(*navigable);
    auto event_name = MUST(String::formatted("network.{}", phase));
    auto scope = session_state().enabled_event_contexts.find(event_name);
    if (scope == session_state().enabled_event_contexts.end() || (scope->value.has_value() && !scope->value->contains_slow(context)))
        return intercepts;
    // 5. For each intercept id → intercept of session intercepts:
    for (auto const& intercept : session_state().network_intercepts) {
        if (intercept.browsing_contexts.has_value() && !intercept.browsing_contexts->contains_slow(context))
            continue;
        if (!intercept.phases.contains_slow(phase))
            continue;
        if (intercept.url_patterns.is_empty() || any_of(intercept.url_patterns, [&](auto const& pattern) { return pattern.matches(request->current_url()); }))
            MUST(intercepts.append(intercept.id));
    }
    return intercepts;
}

NetworkRequest::NetworkRequest()
    : id(generate_random_uuid())
{
}

void initialize_network_request(Request& request, GC::Ptr<HTML::Navigable> navigable, Optional<Utf16String> const& navigation)
{
    if ((!g_network_events_enabled && !g_network_configuration_enabled) || request.webdriver_bidi_network_request())
        return;
    auto state = NetworkRequest::create();
    if (navigable)
        state->navigable_id = navigable->id();
    else if (auto const& client = request.client_snapshot())
        state->navigable_id = client->navigable_id;
    if (navigation.has_value())
        state->navigation = navigation->to_utf8();
    request.set_webdriver_bidi_network_request(state);
}

void synchronize_network_request(Request& original, Request& prepared)
{
    auto state = original.webdriver_bidi_network_request();
    if (!state)
        state = prepared.webdriver_bidi_network_request();
    if (!state) {
        initialize_network_request(original, request_navigable(original));
        state = original.webdriver_bidi_network_request();
    }
    original.set_webdriver_bidi_network_request(state);
    prepared.set_webdriver_bidi_network_request(state);
}

// https://w3c.github.io/webdriver-bidi/#serialize-protocol-bytes
static JsonObject serialize_protocol_bytes(StringView bytes)
{
    JsonObject result;
    if (auto string = String::from_utf8(bytes); !string.is_error()) {
        result.set("type"sv, "string"sv);
        result.set("value"sv, string.release_value());
    } else {
        result.set("type"sv, "base64"sv);
        result.set("value"sv, MUST(encode_base64(bytes.bytes())));
    }
    return result;
}

// https://w3c.github.io/webdriver-bidi/#serialize-header
static JsonArray serialize_headers(HTTP::HeaderList const& headers)
{
    JsonArray result;
    for (auto const& header : headers) {
        JsonObject entry;
        // 1. Let name be the result of UTF-8 decode with name bytes.
        entry.set("name"sv, String::from_utf8_with_replacement_character(header.name.view()));
        // 2. Let value be serialize protocol bytes with value bytes.
        entry.set("value"sv, serialize_protocol_bytes(header.value));
        MUST(result.append(move(entry)));
    }
    return result;
}

static u64 headers_size(HTTP::HeaderList const& headers)
{
    u64 size = 2;
    for (auto const& header : headers)
        size += header.name.length() + header.value.length() + 4;
    return size;
}

// https://w3c.github.io/webdriver-bidi/#get-the-fetch-timings
static JsonObject get_the_fetch_timings(FetchTimingInfo const& timings, Optional<double> time_origin, double epoch_origin)
{
    JsonObject result;
    result.set("timeOrigin"sv, epoch_origin);
    auto convert = [&](double time) {
        if (!time_origin.has_value() || time == 0)
            return 0.0;
        return HighResolutionTime::coarsen_time(time) - *time_origin;
    };
    auto connection = timings.final_connection_timing_info().value_or(ConnectionTimingInfo {});
    result.set("requestTime"sv, convert(timings.start_time()));
    result.set("redirectStart"sv, convert(timings.redirect_start_time()));
    result.set("redirectEnd"sv, convert(timings.redirect_end_time()));
    result.set("fetchStart"sv, convert(timings.post_redirect_start_time()));
    result.set("dnsStart"sv, convert(connection.domain_lookup_start_time));
    result.set("dnsEnd"sv, convert(connection.domain_lookup_end_time));
    result.set("connectStart"sv, convert(connection.connection_start_time));
    result.set("connectEnd"sv, convert(connection.connection_end_time));
    result.set("tlsStart"sv, convert(connection.secure_connection_start_time));
    result.set("requestStart"sv, convert(timings.final_network_request_start_time()));
    result.set("responseStart"sv, convert(timings.final_network_response_start_time()));
    result.set("responseEnd"sv, convert(timings.end_time()));
    return result;
}

static JsonObject get_the_fetch_timings(Request const& request, FetchTimingInfo const& timings)
{
    auto const& client = request.client_snapshot();
    if (!client)
        return get_the_fetch_timings(timings, {}, 0);
    return get_the_fetch_timings(timings, client->time_origin, client->time_origin - HighResolutionTime::estimated_monotonic_time_of_the_unix_epoch());
}

// https://w3c.github.io/webdriver-bidi/#get-the-request-data
static JsonObject get_the_request_data(Request const& request, FetchTimingInfo const& timings)
{
    JsonObject result;
    result.set("request"sv, request.webdriver_bidi_network_request()->id);
    result.set("url"sv, request.current_url().serialize());
    result.set("method"sv, MUST(String::from_utf8(request.method().view())));
    // NB: RequestServer sends one field per name, combining repeated values in order.
    auto headers = HTTP::HeaderList::create();
    for (auto const& header : *request.header_list()) {
        if (!headers->contains(header.name))
            headers->append({ header.name, request.header_list()->get(header.name).release_value() });
    }
    result.set("headers"sv, serialize_headers(headers));
    // FIXME: Include the cookies attached by RequestServer.
    result.set("cookies"sv, JsonArray {});
    result.set("headersSize"sv, headers_size(headers));
    JsonValue body_size;
    if (auto const* bytes = request.body().get_pointer<ByteBuffer>())
        body_size = bytes->size();
    else if (auto const* body = request.body().get_pointer<FetchBody>(); body && body->length().has_value())
        body_size = *body->length();
    result.set("bodySize"sv, move(body_size));
    result.set("destination"sv, request.destination().has_value() ? request_destination_to_string(*request.destination()) : ""sv);
    result.set("initiatorType"sv, request.initiator_type().has_value() ? JsonValue(initiator_type_to_string(*request.initiator_type()).to_utf16_string().to_utf8()) : JsonValue {});
    result.set("timings"sv, get_the_fetch_timings(request, timings));
    return result;
}

// https://w3c.github.io/webdriver-bidi/#extract-challenges
static JsonArray extract_challenges(Response const& response)
{
    JsonArray challenges;
    auto header_name = response.status() == 407 ? "Proxy-Authenticate"sv : "WWW-Authenticate"sv;
    for (auto const& header : *response.header_list()) {
        if (!header.name.equals_ignoring_ascii_case(header_name))
            continue;
        GenericLexer lexer(header.value.view());
        Optional<JsonObject> challenge;
        while (!lexer.is_eof()) {
            lexer.ignore_while([](char ch) { return ch == ' ' || ch == '\t' || ch == ','; });
            auto name = lexer.consume_while([](char ch) { return HTTP::is_header_name({ &ch, 1 }); });
            lexer.ignore_while([](char ch) { return ch == ' ' || ch == '\t'; });
            if (name.is_empty()) {
                if (!lexer.is_eof())
                    lexer.ignore();
                continue;
            }
            if (!lexer.consume_specific('=')) {
                if (challenge.has_value())
                    MUST(challenges.append(challenge.release_value()));
                challenge = JsonObject {};
                challenge->set("scheme"sv, MUST(String::from_utf8(name)));
                challenge->set("realm"sv, ""sv);
                continue;
            }
            lexer.ignore_while([](char ch) { return ch == ' ' || ch == '\t'; });
            StringBuilder value;
            if (lexer.consume_specific('"')) {
                while (!lexer.is_eof() && lexer.peek() != '"') {
                    if (lexer.consume_specific('\\') && lexer.is_eof())
                        break;
                    value.append(lexer.consume());
                }
                lexer.consume_specific('"');
            } else {
                value.append(lexer.consume_until([](char ch) { return ch == ',' || ch == ' ' || ch == '\t'; }));
            }
            if (challenge.has_value() && name.equals_ignoring_ascii_case("realm"sv))
                challenge->set("realm"sv, String::from_utf8_with_replacement_character(value.string_view()));
        }
        if (challenge.has_value())
            MUST(challenges.append(challenge.release_value()));
    }
    return challenges;
}

// https://w3c.github.io/webdriver-bidi/#get-the-response-data
static JsonObject get_the_response_data(Response const& response, FetchTimingInfo const& timings)
{
    JsonObject result;
    auto url = response.url();
    result.set("url"sv, url.has_value() ? url->serialize() : String {});
    ByteString protocol;
    if (timings.final_connection_timing_info().has_value())
        protocol = timings.final_connection_timing_info()->alpn_negotiated_protocol;
    if (protocol.is_empty()) {
        if (auto const& network_transport = response.network_transport(); network_transport.has_value())
            protocol = network_transport->handle->http_version();
    }
    if (protocol.is_empty())
        protocol = url.has_value() ? url->scheme().to_byte_string() : ByteString {};
    if (protocol == "http/2"sv)
        protocol = "h2"sv;
    else if (protocol == "http/3"sv)
        protocol = "h3"sv;
    result.set("protocol"sv, MUST(String::from_utf8(protocol.view())));
    result.set("status"sv, response.status());
    // INTEROP: Data URL responses have an "OK" status text in Chromium and Firefox.
    auto status_text = response.status_message();
    if (status_text.is_empty() && url.has_value() && url->scheme() == "data"sv)
        status_text = "OK"sv;
    result.set("statusText"sv, String::from_utf8_with_replacement_character(status_text.view()));
    result.set("fromCache"sv, response.cache_state() == Response::CacheState::Local);
    result.set("headers"sv, serialize_headers(response.header_list()));
    auto mime_type = extract_mime_type(response.header_list());
    result.set("mimeType"sv, mime_type.has_value() ? mime_type->essence() : String {});
    auto body_info = response.body_info();
    if (auto body = response.body(); body.has_value() && body->source().has_value() && body_info.decoded_size == 0) {
        body_info.decoded_size = body->source()->size();
        if (url.has_value() && url->scheme() == "data"sv)
            body_info.encoded_size = body_info.decoded_size;
    }
    auto header_size = headers_size(response.header_list());
    result.set("bytesReceived"sv, body_info.encoded_size + header_size);
    result.set("headersSize"sv, header_size);
    result.set("bodySize"sv, body_info.encoded_size);
    JsonObject content;
    content.set("size"sv, body_info.decoded_size);
    result.set("content"sv, move(content));
    if (response.status() == 401 || response.status() == 407)
        result.set("authChallenges"sv, extract_challenges(response));
    return result;
}

// https://w3c.github.io/webdriver-bidi/#process-a-network-event
static JsonObject process_a_network_event(HTML::Navigable const& navigable, Request const& request, FetchTimingInfo const& timing_info)
{
    auto state = request.webdriver_bidi_network_request();
    JsonObject params;
    params.set("context"sv, navigable_id(navigable));
    params.set("userContext"sv, "default"sv);
    auto blocked = blocked_request(request);
    params.set("isBlocked"sv, blocked != nullptr);
    if (blocked)
        params.set("intercepts"sv, blocked->intercepts);
    params.set("navigation"sv, state->navigation.has_value() ? JsonValue(*state->navigation) : JsonValue {});
    params.set("redirectCount"sv, request.redirect_count());
    JsonObject request_data;
    if (auto cached = state->request_data.get(request.redirect_count()); cached.has_value())
        request_data = *cached;
    else
        request_data = get_the_request_data(request, timing_info);
    request_data.set("timings"sv, get_the_fetch_timings(request, timing_info));
    params.set("request"sv, move(request_data));
    params.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
    return params;
}

void block_network_request(FetchParams const& fetch_params, RefPtr<Fetch::Infrastructure::Response> response, String phase, JsonArray intercepts, GC::Ref<NetworkContinuation> continuation, RefPtr<Fetch::Infrastructure::Request> original_request)
{
    auto request = fetch_params.request();
    // NB: Cancellation runs through the controller's original parameters, rather than a prepared HTTP clone.
    //     Bind both requests to the same identity before exposing a blocked request.
    if (auto original_params = fetch_params.controller()->original_fetch_params()) {
        synchronize_network_request(*original_params->request(), *request);
        if (!original_request)
            original_request = original_params->request();
    }
    initialize_network_request(*request);
    auto state = request->webdriver_bidi_network_request();
    if (fetch_params.is_canceled() || fetch_params.control_block().state() == FetchControlBlock::State::Stopped) {
        if (response)
            stop_response_transport(*fetch_params.controller(), *response);
        continuation->invoke(NetworkContinuationAction::Fail, Response::network_error("Intercepted fetch was stopped"_string));
        return;
    }
    // NB: Allocating the continuation after selecting intercepts can collect an otherwise unreachable origin.
    //     Continue the fetch without blocking when that context has gone away.
    if (!state || !navigable_of(*state)) {
        continuation->invoke(NetworkContinuationAction::Continue, response);
        return;
    }
    VERIFY(!blocked_requests().contains(state->id));
    continuation->request_id = state->id;
    continuation->phase = move(phase);
    continuation->intercepts = move(intercepts);
    continuation->fetch_params = fetch_params;
    continuation->controller = fetch_params.controller();
    continuation->original_request = original_request;
    continuation->response = response;
    if (original_request)
        original_request->set_webdriver_bidi_network_request(state);
    blocked_requests().set(state->id, GC::make_root(continuation));
    if (continuation->phase == "beforeRequestSent"sv) {
        network_before_request_sent(fetch_params);
    } else if (continuation->phase == "responseStarted"sv) {
        network_response_started(fetch_params, *response);
    } else {
        network_auth_required(fetch_params, *response);
    }
}

// https://w3c.github.io/webdriver-bidi/#command-network-continueRequest
// https://w3c.github.io/webdriver-bidi/#update-the-response
static ErrorOr<JsonValue, Error> continue_network_request_impl(StringView method, JsonObject const& params)
{
    TRY(Network::validate_interception_parameters(method, params));
    // 1. Let blocked requests be session's blocked request map.
    // 2. Let request id be command parameters["request"].
    auto id = params.get_string("request"sv).release_value();
    // 3. If blocked requests does not contain request id then return error with error code no such request.
    auto it = blocked_requests().find(id);
    if (it == blocked_requests().end())
        return Error::from_code(ErrorCode::NoSuchRequest, "Blocked request not found"sv);
    GC::Ptr<NetworkContinuation> blocked = it->value.ptr();
    auto phase = blocked->phase;
    if ((method == "network.continueRequest"sv && phase != "beforeRequestSent"sv)
        || (method == "network.continueResponse"sv && phase == "beforeRequestSent"sv)
        || (method == "network.continueWithAuth"sv && phase != "authRequired"sv)
        || (method == "network.failRequest"sv && phase == "authRequired"sv))
        return Error::from_code(ErrorCode::InvalidArgument, "Command does not apply to the blocked phase"sv);
    auto overrides = TRY(Network::deserialize_interception_overrides(method, params));
    auto request = blocked->fetch_params->request();
    auto state = request->webdriver_bidi_network_request();
    auto response = blocked->response;
    auto action = NetworkContinuationAction::Continue;
    if (method == "network.continueRequest"sv) {
        // NB: A supplied header list may include Fetch-generated fields echoed from the event. Remember those
        //     explicit values separately, so preparation can restore them without appending duplicate fields.
        if (overrides.headers.has_value()) {
            state->request_header_overrides = HTTP::HeaderList::create();
            for (auto const& header : *overrides.headers) {
                if (any_of(fetch_generated_request_header_names, [&](auto name) { return header.name.equals_ignoring_ascii_case(name); }))
                    state->request_header_overrides->append(header);
            }
        }
        auto original_request = blocked->original_request;
        if (original_request && original_request != request) {
            if (overrides.url.has_value())
                original_request->url_list().last() = *overrides.url;
            if (overrides.method.has_value())
                original_request->set_method(*overrides.method);
            if (overrides.headers.has_value()) {
                auto headers = HTTP::HeaderList::create(*overrides.headers);
                for (auto name : fetch_generated_request_header_names)
                    headers->delete_(name);
                original_request->set_header_list(move(headers));
            }
            if (overrides.cookie_header.has_value())
                original_request->header_list()->set({ "Cookie"sv, *overrides.cookie_header });
        }
        if (overrides.url.has_value())
            request->url_list().last() = overrides.url.release_value();
        if (overrides.method.has_value())
            request->set_method(overrides.method.release_value());
        if (overrides.headers.has_value())
            request->set_header_list(HTTP::HeaderList::create(overrides.headers.release_value()));
        if (overrides.cookie_header.has_value())
            request->header_list()->set({ "Cookie"sv, overrides.cookie_header.release_value() });
        if (overrides.body.has_value()) {
            if (state->request_header_overrides)
                state->request_header_overrides->delete_("Content-Length"sv);
            if (original_request)
                original_request->header_list()->delete_("Content-Length"sv);
            auto body = FetchBody::from_bytes(Core::ImmutableBytes::adopt(overrides.body.release_value()));
            request->set_body(body);
            if (original_request && original_request != request)
                original_request->set_body(body);
            request->header_list()->set({ "Content-Length"sv, ByteString::number(*body.length()) });
        }

        state->request_data.set(request->redirect_count(), get_the_request_data(*request, *blocked->fetch_params->timing_info()));
        if (params.has("url"sv) || params.has("method"sv) || params.has("headers"sv) || params.has("body"sv) || params.has("cookies"sv))
            action = NetworkContinuationAction::ContinueWithModifiedRequest;
    } else if (method == "network.failRequest"sv) {
        action = NetworkContinuationAction::Fail;
        response = Fetch::Infrastructure::Response::network_error("Request failed by WebDriver BiDi"_string);
    } else if (method != "network.continueWithAuth"sv) {
        if (!response) {
            response = Fetch::Infrastructure::Response::create();
            response->set_url_list(request->url_list());
        }
        // 7. If command parameters contains "statusCode":
        if (overrides.status.has_value())
            response->set_status(*overrides.status);
        // 8. If command parameters contains "reasonPhrase":
        if (overrides.reason.has_value())
            response->set_status_message(overrides.reason.release_value());
        // 9. If command parameters contains "headers":
        if (overrides.headers.has_value())
            response->set_header_list(HTTP::HeaderList::create(overrides.headers.release_value()));
        if (overrides.set_cookie_headers.has_value()) {
            if (!params.has("headers"sv)) {
                response->set_header_list(HTTP::HeaderList::create(response->header_list()->headers()));
                response->header_list()->delete_("Set-Cookie"sv);
            }
            for (auto const& header : *overrides.set_cookie_headers)
                response->header_list()->append(header);
        }
        if (method == "network.provideResponse"sv)
            action = NetworkContinuationAction::ProvideResponse;
        if (method == "network.provideResponse"sv && overrides.body.has_value()) {
            // NB: A replacement body has no RequestServer transport to transfer to a new navigation process.
            if (blocked->response) {
                blocked->response->release_request_transfer_lease();
                blocked->controller->stop_body_transport();
            }
            auto replacement = Response::create();
            replacement->set_url_list(response->url_list());
            replacement->set_status(response->status());
            replacement->set_status_message(response->status_message());
            replacement->set_header_list(response->header_list());
            response = replacement;
            auto body = ResponseBody::from_bytes(Core::ImmutableBytes::adopt(overrides.body.release_value()));
            auto length = *body.length();
            response->set_body(move(body));
            auto info = response->body_info();
            info.encoded_size = length;
            info.decoded_size = length;
            response->set_body_info(info);
        }
    }
    if (phase == "authRequired"sv && method != "network.failRequest"sv && method != "network.provideResponse"sv) {
        action = NetworkContinuationAction::UseDefaultAuthentication;
        if (method == "network.continueWithAuth"sv) {
            auto auth_action = params.get_string("action"sv).release_value();
            if (auth_action == "default"sv)
                action = NetworkContinuationAction::UseDefaultAuthentication;
            else if (auth_action == "cancel"sv)
                action = NetworkContinuationAction::CancelAuthentication;
            else
                action = NetworkContinuationAction::ProvideCredentials;
        }
        if (auto credentials = params.get_object("credentials"sv); credentials.has_value() && (method == "network.continueResponse"sv || action == NetworkContinuationAction::ProvideCredentials)) {
            auto payload = MUST(String::formatted("{}:{}", *credentials->get_string("username"sv), *credentials->get_string("password"sv)));
            request->header_list()->set({ "Authorization"sv, ByteString::formatted("Basic {}", MUST(encode_base64(payload.bytes()))) });
            action = NetworkContinuationAction::ProvideCredentials;
        }
    }
    resume_blocked_request(id, action, response);
    return JsonValue {};
}

WebDriver::Response continue_network_request(StringView method, JsonObject const& params)
{
    auto result = continue_network_request_impl(method, params);
    if (result.is_error())
        return result.release_error();
    return result.release_value();
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-before-request-sent
void network_before_request_sent(FetchParams const& fetch_params)
{
    share_blocked_request_identity(fetch_params);
    auto request = fetch_params.request();
    initialize_network_request(*request);
    auto state = request->webdriver_bidi_network_request();
    if (!state || state->before_request_sent.contains(request->redirect_count()))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    // 4. Let redirect count be request's redirect count.
    // 5. Add redirect count to before request sent map[request].
    state->before_request_sent.set(request->redirect_count());
    state->request_data.set(request->redirect_count(), get_the_request_data(*request, *fetch_params.timing_info()));
    if (is_event_enabled("network.beforeRequestSent"sv))
        emit_event(*navigable, "network.beforeRequestSent"_string, process_a_network_event(*navigable, *fetch_params.request(), *fetch_params.timing_info()));
}

// https://w3c.github.io/webdriver-bidi/#event-network-responseStarted
void network_response_started(FetchParams const& fetch_params, Response& response)
{
    network_before_request_sent(fetch_params);
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    if (!state || state->response_started.contains(request->redirect_count()) || response.is_network_error())
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    state->response_started.set(request->redirect_count());
    // 5. Let sessions be the set of sessions for which an event is enabled given "network.responseStarted" and related navigables.
    if (is_event_enabled("network.responseStarted"sv)) {
        auto params = process_a_network_event(*navigable, *fetch_params.request(), *fetch_params.timing_info());
        params.set("response"sv, get_the_response_data(response, *fetch_params.timing_info()));
        emit_event(*navigable, "network.responseStarted"_string, move(params));
    }
}

// https://w3c.github.io/webdriver-bidi/#event-network-authRequired
void network_auth_required(FetchParams const& fetch_params, Response const& response)
{
    auto state = fetch_params.request()->webdriver_bidi_network_request();
    if (!state || !is_event_enabled("network.authRequired"sv))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    auto params = process_a_network_event(*navigable, *fetch_params.request(), *fetch_params.timing_info());
    params.set("response"sv, get_the_response_data(response, *fetch_params.timing_info()));
    emit_event(*navigable, "network.authRequired"_string, move(params));
}

// https://w3c.github.io/webdriver-bidi/#event-network-responseCompleted
void network_response_completed(FetchParams const& fetch_params, Response const& response)
{
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    if (!state || (state->completed.contains(request->redirect_count()) || state->transferred.contains(request->redirect_count())))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    state->completed.set(request->redirect_count());
    if (!fetch_params.timing_info()->end_time())
        fetch_params.timing_info()->set_end_time(HighResolutionTime::unsafe_shared_current_time());
    // 5. Let sessions be the set of sessions for which an event is enabled given "network.responseCompleted" and related navigables.
    if (!is_event_enabled("network.responseCompleted"sv))
        return;
    auto params = process_a_network_event(*navigable, *fetch_params.request(), *fetch_params.timing_info());
    params.set("response"sv, get_the_response_data(response, *fetch_params.timing_info()));
    emit_event(*navigable, "network.responseCompleted"_string, move(params));
}

// https://w3c.github.io/webdriver-bidi/#event-network-fetchError
void network_fetch_error(FetchParams const& fetch_params, StringView error_text)
{
    // 1. If before request sent map[request] does not contain request's redirect count, then run the WebDriver BiDi before request sent steps with request.
    network_before_request_sent(fetch_params);
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    bool is_first_terminal = state && !state->completed.contains(request->redirect_count()) && !state->transferred.contains(request->redirect_count());
    if (is_first_terminal)
        state->completed.set(request->redirect_count());
    cancel_network_interception_impl(fetch_params, error_text);
    if (!is_first_terminal)
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    if (!is_event_enabled("network.fetchError"sv))
        return;
    auto params = process_a_network_event(*navigable, *fetch_params.request(), *fetch_params.timing_info());
    // 4.2. Set the errorText field of params to an implementation-defined string describing the error which caused the request to be aborted.
    params.set("errorText"sv, String::from_utf8_with_replacement_character(error_text));
    emit_event(*navigable, "network.fetchError"_string, move(params));
}

Optional<JsonObject> network_response_for_navigation(Request const& request, FetchTimingInfo const& timing_info,
    Response const& response)
{
    if (!g_network_events_enabled)
        return {};
    auto state = request.webdriver_bidi_network_request();
    if (!state || state->completed.contains(request.redirect_count()))
        return {};
    auto navigable = navigable_of(*state);
    if (!navigable)
        return {};
    state->transferred.set(request.redirect_count());
    JsonObject result;
    result.set("parameters"sv, process_a_network_event(*navigable, request, timing_info));
    result.set("response"sv, get_the_response_data(response, timing_info));
    result.set("postRedirectStartTime"sv, timing_info.post_redirect_start_time());
    auto const& client = request.client_snapshot();
    result.set("timeOrigin"sv, client ? JsonValue(client->time_origin) : JsonValue {});
    return result;
}

void network_navigation_response_finished(HTML::Navigable const& navigable, JsonObject const& metadata, Response const& response, FetchTimingInfo const* timing_info, Optional<StringView> error)
{
    auto name = error.has_value() ? "network.fetchError"sv : "network.responseCompleted"sv;
    if (!g_network_events_enabled || !is_event_enabled(name))
        return;
    auto parameters = metadata.get_object("parameters"sv);
    if (!parameters.has_value())
        return;
    auto params = *parameters;
    params.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
    if (error.has_value()) {
        params.set("errorText"sv, String::from_utf8_with_replacement_character(*error));
    } else {
        auto fallback_timings = FetchTimingInfo::create();
        auto const& timings = timing_info ? *timing_info : *fallback_timings;
        if (auto request = params.get_object("request"sv); request.has_value()) {
            auto old_timings = request->get_object("timings"sv);
            auto epoch_origin = old_timings->get("timeOrigin"sv)->get_double_with_precision_loss().value();
            auto origin = metadata.get("timeOrigin"sv);
            request->set("timings"sv, get_the_fetch_timings(timings, origin->is_null() ? Optional<double> {} : origin->get_double_with_precision_loss(), epoch_origin));
        }
        params.set("response"sv, get_the_response_data(response, timings));
    }
    emit_event(navigable, MUST(String::from_utf8(name)), move(params));
}

}
