/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/GenericLexer.h>
#include <AK/JsonArray.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Infrastructure/ClientContextSnapshot.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/Network.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>

namespace Web::WebDriver::BiDi {

using namespace Fetch::Infrastructure;
using Response = Fetch::Infrastructure::Response;

static thread_local bool g_network_events_enabled { false };

bool network_events_enabled()
{
    return g_network_events_enabled;
}

// https://w3c.github.io/webdriver-bidi/#blocked-request-map
// NB: The fetch of a blocked request waits for the network interception its engine offered for it.
struct BlockedRequest {
    WeakPtr<FetchControllerRegistry> registry;
    FetchControllerToken controller;
    Optional<Fetch::Engine::CorsPreflightId> cors_preflight_id;
    Fetch::Engine::ContinuationId interception;
    Fetch::Engine::NetworkPhase phase;
};

static HashMap<String, BlockedRequest>& blocked_requests()
{
    static thread_local NeverDestroyed<HashMap<String, BlockedRequest>> requests;
    return *requests;
}

static void resume_blocked_request(String const& id, Fetch::Engine::NetworkDecision decision)
{
    auto blocked = blocked_requests().take(id);
    if (!blocked.has_value())
        return;
    if (auto controller = blocked->registry ? blocked->registry->find(blocked->controller) : nullptr)
        controller->resolve_network_interception(blocked->interception, move(decision));
}

static StringView phase_name(Fetch::Engine::NetworkPhase phase)
{
    switch (phase) {
    case Fetch::Engine::NetworkPhase::BeforeRequestSent:
        return "beforeRequestSent"sv;
    case Fetch::Engine::NetworkPhase::ResponseStarted:
        return "responseStarted"sv;
    case Fetch::Engine::NetworkPhase::AuthRequired:
        return "authRequired"sv;
    }
    VERIFY_NOT_REACHED();
}

static RefPtr<Fetch::Engine::NetworkConfiguration const> network_configuration(BiDiSessionState const& state)
{
    if (!state.is_active)
        return nullptr;
    if (state.network_default_headers.is_empty() && state.network_user_context_headers.is_empty() && state.network_navigable_headers.is_empty() && state.network_default_cache_behavior == "default"sv && state.network_navigable_cache_behavior.is_empty())
        return nullptr;

    auto configuration = make_ref_counted<Fetch::Engine::NetworkConfiguration>();
    configuration->bypass_cache_by_default = state.network_default_cache_behavior == "bypass"sv;
    for (auto const& [traversable, behavior] : state.network_navigable_cache_behavior)
        configuration->bypass_cache_by_traversable.set(traversable, behavior == "bypass"sv);
    configuration->default_headers = state.network_default_headers;
    if (auto headers = state.network_user_context_headers.get("default"_string); headers.has_value())
        configuration->default_user_context_headers = *headers;
    configuration->traversable_headers = state.network_navigable_headers;
    return configuration;
}

void network_session_state_changed()
{
    auto const& state = session_state();
    g_network_events_enabled = state.is_active && any_of(state.enabled_events, [](auto const& name) { return name.starts_with_bytes("network."sv); });

    Vector<Fetch::Engine::NetworkPhase, 3> intercepted_phases;
    if (g_network_events_enabled) {
        for (auto phase : { Fetch::Engine::NetworkPhase::BeforeRequestSent, Fetch::Engine::NetworkPhase::ResponseStarted, Fetch::Engine::NetworkPhase::AuthRequired }) {
            auto name = phase_name(phase);
            auto has_intercept = any_of(state.network_intercepts, [&](auto const& intercept) { return intercept.phases.contains_slow(name); });
            if (has_intercept && is_event_enabled(MUST(String::formatted("network.{}", name))))
                intercepted_phases.append(phase);
        }
    }
    auto& network_activity_observers = *HTML::surrounding_agent().network_activity_observers;
    network_activity_observers.set_fetch_observation(g_network_events_enabled, intercepted_phases);
    network_activity_observers.set_network_configuration(network_configuration(state));

    if (!state.is_active) {
        // https://w3c.github.io/webdriver-bidi/#cleanup-the-session
        // 3. For each request id → (request, phase, response) in session's blocked request map:
        while (!blocked_requests().is_empty()) {
            // 1. Resume with "continue request", request id and (response, "incomplete").
            auto id = blocked_requests().begin()->key;
            resume_blocked_request(id, Fetch::Engine::ContinueFetch {});
        }
    }
}

static GC::Ptr<HTML::LocalNavigable> navigable_of(NetworkRequest const& state)
{
    if (!state.navigable_id.has_value())
        return {};
    return HTML::local_navigable_with_id(*state.navigable_id);
}

static String network_context_id(HTML::Navigable& navigable)
{
    return Fetch::Engine::NetworkConfiguration::traversable_name(navigable.top_level_traversable()->id());
}

// The request of a fetch, or of one of its CORS preflights, that a network event reports.
class ObservedRequestSource {
public:
    ObservedRequestSource(FetchController& controller, Optional<Fetch::Engine::CorsPreflightId> cors_preflight_id)
        : m_controller(controller)
        , m_observation(cors_preflight_id.has_value() ? RefPtr { controller.cors_preflight_network_observation(*cors_preflight_id) } : controller.network_observation())
        , m_cors_preflight_id(cors_preflight_id)
    {
    }

    [[nodiscard]] FetchController& controller() const { return m_controller; }
    [[nodiscard]] Optional<Fetch::Engine::CorsPreflightId> cors_preflight_id() const { return m_cors_preflight_id; }

    [[nodiscard]] RefPtr<NetworkRequest> network_request() const
    {
        if (!m_observation)
            return nullptr;
        return static_cast<NetworkRequest*>(m_observation->observer_state().ptr());
    }

    // The request's state, which the first event that reports the request creates, for a request with a navigable.
    RefPtr<NetworkRequest> ensure_network_request(Fetch::Engine::Event::ObservedRequest const& request) const
    {
        if (auto state = network_request())
            return state;
        if (!m_observation || !request.navigable_id.has_value() || !HTML::local_navigable_with_id(*request.navigable_id))
            return nullptr;
        auto state = NetworkRequest::create();
        state->navigable_id = request.navigable_id;
        state->navigation = request.navigation_id;
        // NB: A CORS preflight's request has the WebDriver id of the request it is for.
        if (m_cors_preflight_id.has_value()) {
            auto const& fetch_request = m_controller->observed_request();
            auto fetch_state = ObservedRequestSource { m_controller, {} }.ensure_network_request(fetch_request.value_or(request));
            if (!fetch_state)
                return nullptr;
            state->id = fetch_state->id;
        }
        m_observation->set_observer_state(state);
        return state;
    }

    // The end time the agent that started the fetch set last, which the events of a CORS preflight report as 0 until it
    // completes.
    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp end_time() const
    {
        return m_cors_preflight_id.has_value() ? 0 : m_controller->control_block().end_time();
    }

    // The end time a response's completion reports, which it sets for a fetch with none yet.
    HighResolutionTime::DOMHighResTimeStamp end_time_at_completion() const
    {
        if (m_cors_preflight_id.has_value())
            return HighResolutionTime::unsafe_shared_current_time();
        auto& control_block = m_controller->control_block();
        if (!control_block.end_time())
            control_block.set_end_time(HighResolutionTime::unsafe_shared_current_time());
        return control_block.end_time();
    }

private:
    GC::Ref<FetchController> m_controller;
    RefPtr<NetworkObservation> m_observation;
    Optional<Fetch::Engine::CorsPreflightId> m_cors_preflight_id;
};

static void end_network_interception(NetworkRequest const& state, Fetch::Engine::ContinuationId interception)
{
    auto navigable = navigable_of(state);
    if (!navigable)
        return;
    JsonObject request;
    request.set("request"sv, state.id);
    JsonObject params;
    params.set("request"sv, move(request));
    params.set(Network::network_interception_parameter, interception.value());
    emit_event(*navigable, MUST(String::from_utf8(Network::network_interception_ended_event)), move(params));
}

static void forget_blocked_requests(FetchController& controller)
{
    auto state = ObservedRequestSource { controller, {} }.network_request();
    Optional<Fetch::Engine::ContinuationId> interception;
    blocked_requests().remove_all_matching([&](auto const& id, auto const& blocked) {
        if (blocked.controller != controller.token())
            return false;
        if (state && id == state->id)
            interception = blocked.interception;
        return true;
    });
    if (interception.has_value())
        end_network_interception(*state, *interception);
}

static NonnullRefPtr<FetchTimingInfo> timing_info_of(Fetch::Engine::Event::ObservedRequest const& request)
{
    auto timing_info = FetchTimingInfo::create(request.timing_seed);
    if (request.network_phases.has_value())
        timing_info->set_network_phases(*request.network_phases);
    return timing_info;
}

// https://w3c.github.io/webdriver-bidi/#get-the-network-intercepts
static JsonArray get_network_intercepts(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, StringView phase)
{
    JsonArray intercepts;
    auto event_name = MUST(String::formatted("network.{}", phase));
    if (session_state().network_intercepts.is_empty() || !is_event_enabled(event_name))
        return intercepts;
    if (auto state = source.network_request(); state && ((phase == "beforeRequestSent"sv && state->before_request_sent.contains(request.redirect_count)) || (phase == "responseStarted"sv && state->response_started.contains(request.redirect_count))))
        return intercepts;
    if (!source.controller().control_block().is_ongoing())
        return intercepts;
    auto state = source.ensure_network_request(request);
    auto navigable = state ? navigable_of(*state) : nullptr;
    if (!navigable)
        return intercepts;
    auto context = network_context_id(*navigable);
    auto scope = session_state().enabled_event_contexts.find(event_name);
    if (scope == session_state().enabled_event_contexts.end() || (scope->value.has_value() && !scope->value->contains_slow(context)))
        return intercepts;
    // 5. For each intercept id → intercept of session intercepts:
    for (auto const& intercept : session_state().network_intercepts) {
        if (intercept.browsing_contexts.has_value() && !intercept.browsing_contexts->contains_slow(context))
            continue;
        if (!intercept.phases.contains_slow(phase))
            continue;
        if (intercept.url_patterns.is_empty() || any_of(intercept.url_patterns, [&](auto const& pattern) { return pattern.matches(request.url); }))
            MUST(intercepts.append(intercept.id));
    }
    return intercepts;
}

NetworkRequest::NetworkRequest()
    : id(generate_random_uuid())
{
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
static JsonObject get_the_fetch_timings(FetchTimingInfo const& timings, HighResolutionTime::DOMHighResTimeStamp end_time, Optional<double> time_origin, double epoch_origin)
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
    result.set("responseEnd"sv, convert(end_time));
    return result;
}

static JsonObject get_the_fetch_timings(Fetch::Engine::Event::ObservedRequest const& request, FetchTimingInfo const& timings, HighResolutionTime::DOMHighResTimeStamp end_time)
{
    if (!request.time_origin.has_value())
        return get_the_fetch_timings(timings, end_time, {}, 0);
    return get_the_fetch_timings(timings, end_time, request.time_origin, *request.time_origin - HighResolutionTime::estimated_monotonic_time_of_the_unix_epoch());
}

// https://w3c.github.io/webdriver-bidi/#get-the-request-data
static JsonObject get_the_request_data(NetworkRequest const& state, Fetch::Engine::Event::ObservedRequest const& request, FetchTimingInfo const& timings, HighResolutionTime::DOMHighResTimeStamp end_time)
{
    JsonObject result;
    result.set("request"sv, state.id);
    result.set("url"sv, request.url.serialize());
    result.set("method"sv, MUST(String::from_utf8(request.method.view())));
    // NB: RequestServer sends one field per name, combining repeated values in order.
    auto request_headers = HTTP::HeaderList::create(request.headers);
    auto headers = HTTP::HeaderList::create();
    for (auto const& header : *request_headers) {
        if (!headers->contains(header.name))
            headers->append({ header.name, request_headers->get(header.name).release_value() });
    }
    result.set("headers"sv, serialize_headers(headers));
    // FIXME: Include the cookies attached by RequestServer.
    result.set("cookies"sv, JsonArray {});
    result.set("headersSize"sv, headers_size(headers));
    result.set("bodySize"sv, request.body_size.has_value() ? JsonValue(*request.body_size) : JsonValue {});
    result.set("destination"sv, request.destination);
    result.set("initiatorType"sv, request.initiator_type.has_value() ? JsonValue(*request.initiator_type) : JsonValue {});
    result.set("timings"sv, get_the_fetch_timings(request, timings, end_time));
    return result;
}

// https://w3c.github.io/webdriver-bidi/#extract-challenges
static JsonArray extract_challenges(Fetch::Engine::Event::ObservedResponse const& response)
{
    JsonArray challenges;
    auto header_name = response.status == 407 ? "Proxy-Authenticate"sv : "WWW-Authenticate"sv;
    for (auto const& header : response.headers) {
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
static JsonObject get_the_response_data(Fetch::Engine::Event::ObservedResponse const& response, FetchTimingInfo const& timings)
{
    JsonObject result;
    auto const& url = response.url;
    result.set("url"sv, url.has_value() ? url->serialize() : String {});
    ByteString protocol;
    if (timings.final_connection_timing_info().has_value())
        protocol = timings.final_connection_timing_info()->alpn_negotiated_protocol;
    if (protocol.is_empty())
        protocol = response.protocol;
    if (protocol.is_empty())
        protocol = url.has_value() ? url->scheme().to_byte_string() : ByteString {};
    if (protocol == "http/2"sv)
        protocol = "h2"sv;
    else if (protocol == "http/3"sv)
        protocol = "h3"sv;
    result.set("protocol"sv, MUST(String::from_utf8(protocol.view())));
    result.set("status"sv, response.status);
    // INTEROP: Data URL responses have an "OK" status text in Chromium and Firefox.
    auto status_text = response.status_message;
    if (status_text.is_empty() && url.has_value() && url->scheme() == "data"sv)
        status_text = "OK"sv;
    result.set("statusText"sv, String::from_utf8_with_replacement_character(status_text.view()));
    result.set("fromCache"sv, response.from_cache);
    auto headers = HTTP::HeaderList::create(response.headers);
    result.set("headers"sv, serialize_headers(headers));
    auto mime_type = extract_mime_type(headers);
    result.set("mimeType"sv, mime_type.has_value() ? mime_type->essence() : String {});
    auto header_size = headers_size(headers);
    result.set("bytesReceived"sv, response.encoded_body_size + header_size);
    result.set("headersSize"sv, header_size);
    result.set("bodySize"sv, response.encoded_body_size);
    JsonObject content;
    content.set("size"sv, response.decoded_body_size);
    result.set("content"sv, move(content));
    if (response.status == 401 || response.status == 407)
        result.set("authChallenges"sv, extract_challenges(response));
    return result;
}

// https://w3c.github.io/webdriver-bidi/#process-a-network-event
static JsonObject process_a_network_event(HTML::Navigable const& navigable, NetworkRequest const& state, Fetch::Engine::Event::ObservedRequest const& request, FetchTimingInfo const& timing_info, HighResolutionTime::DOMHighResTimeStamp end_time, JsonArray const& intercepts = {})
{
    JsonObject params;
    params.set("context"sv, navigable_id(navigable));
    params.set("userContext"sv, "default"sv);

    // 7. Let intercepts be the result of get the network intercepts with session, event, request, and top-level
    //    navigable id.
    // NB: The caller gives the intercepts that blocked the request at the event's phase, if any.

    // 10. If intercepts is not empty, let is blocked be true, otherwise let is blocked be false.
    auto is_blocked = !intercepts.is_empty();
    params.set("isBlocked"sv, is_blocked);
    if (is_blocked) {
        params.set("intercepts"sv, intercepts);
        params.set(Network::network_interception_parameter, blocked_requests().get(state.id)->interception.value());
    }

    params.set("navigation"sv, state.navigation.has_value() ? JsonValue(*state.navigation) : JsonValue {});
    params.set("redirectCount"sv, request.redirect_count);
    JsonObject request_data;
    if (auto cached = state.request_data.get(request.redirect_count); cached.has_value())
        request_data = *cached;
    else
        request_data = get_the_request_data(state, request, timing_info, end_time);
    request_data.set("timings"sv, get_the_fetch_timings(request, timing_info, end_time));
    params.set("request"sv, move(request_data));
    params.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
    return params;
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-before-request-sent
static void network_before_request_sent(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, FetchTimingInfo const& timing_info, JsonArray const& intercepts = {})
{
    auto state = source.ensure_network_request(request);
    if (!state || state->before_request_sent.contains(request.redirect_count))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;

    // 4. Let redirect count be request's redirect count.
    // 5. Add redirect count to before request sent map[request].
    state->before_request_sent.set(request.redirect_count);
    state->request_data.set(request.redirect_count, get_the_request_data(*state, request, timing_info, source.end_time()));
    if (is_event_enabled("network.beforeRequestSent"sv))
        emit_event(*navigable, "network.beforeRequestSent"_string, process_a_network_event(*navigable, *state, request, timing_info, source.end_time(), intercepts));
}

// https://w3c.github.io/webdriver-bidi/#event-network-responseStarted
static void network_response_started(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, Fetch::Engine::Event::ObservedResponse const& response, FetchTimingInfo const& timing_info, JsonArray const& intercepts)
{
    network_before_request_sent(source, request, timing_info);
    auto state = source.network_request();
    if (!state || state->response_started.contains(request.redirect_count))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    state->response_started.set(request.redirect_count);

    // 5. Let sessions be the set of sessions for which an event is enabled given "network.responseStarted" and related
    //    navigables.
    if (is_event_enabled("network.responseStarted"sv)) {
        auto params = process_a_network_event(*navigable, *state, request, timing_info, source.end_time(), intercepts);
        params.set("response"sv, get_the_response_data(response, timing_info));
        emit_event(*navigable, "network.responseStarted"_string, move(params));
    }
}

// https://w3c.github.io/webdriver-bidi/#event-network-authRequired
static void network_auth_required(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, Fetch::Engine::Event::ObservedResponse const& response, FetchTimingInfo const& timing_info, JsonArray const& intercepts)
{
    auto state = source.network_request();
    if (!state || !is_event_enabled("network.authRequired"sv))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    auto params = process_a_network_event(*navigable, *state, request, timing_info, source.end_time(), intercepts);
    params.set("response"sv, get_the_response_data(response, timing_info));
    emit_event(*navigable, "network.authRequired"_string, move(params));
}

// https://w3c.github.io/webdriver-bidi/#event-network-responseCompleted
static void network_response_completed(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, Fetch::Engine::Event::ObservedResponse const& response, FetchTimingInfo const& timing_info)
{
    auto state = source.network_request();
    if (!state || state->completed.contains(request.redirect_count) || state->transferred.contains(request.redirect_count))
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;
    state->completed.set(request.redirect_count);
    auto end_time = source.end_time_at_completion();

    // 5. Let sessions be the set of sessions for which an event is enabled given "network.responseCompleted" and
    //    related navigables.
    if (!is_event_enabled("network.responseCompleted"sv))
        return;
    auto params = process_a_network_event(*navigable, *state, request, timing_info, end_time);
    params.set("response"sv, get_the_response_data(response, timing_info));
    emit_event(*navigable, "network.responseCompleted"_string, move(params));
}

// https://w3c.github.io/webdriver-bidi/#event-network-fetchError
static void network_fetch_error(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, FetchTimingInfo const& timing_info, StringView error_text)
{
    // 1. If before request sent map[request] does not contain request's redirect count, then run the WebDriver BiDi
    //    before request sent steps with request.
    network_before_request_sent(source, request, timing_info);
    auto state = source.network_request();
    bool is_first_terminal = state && !state->completed.contains(request.redirect_count) && !state->transferred.contains(request.redirect_count);
    if (is_first_terminal)
        state->completed.set(request.redirect_count);
    // NB: An error fails the fetch if the session blocks it.
    if (state) {
        auto blocked = blocked_requests().get(state->id);
        if (blocked.has_value() && blocked->controller == source.controller().token() && blocked->cors_preflight_id == source.cors_preflight_id()) {
            end_network_interception(*state, blocked->interception);
            resume_blocked_request(state->id, Fetch::Engine::FailFetch {});
        }
    }
    if (!is_first_terminal)
        return;
    auto navigable = navigable_of(*state);
    if (!navigable)
        return;

    // 4. For each session in the set of sessions for which an event is enabled given "network.fetchError" and related
    //    navigables:
    if (!is_event_enabled("network.fetchError"sv))
        return;

    // 1. Let params be the result of process a network event with session "network.fetchError", and request.
    auto params = process_a_network_event(*navigable, *state, request, timing_info, source.end_time());

    // 2. Set the errorText field of params to an implementation-defined string describing the error which caused the
    //    request to be aborted.
    params.set("errorText"sv, String::from_utf8_with_replacement_character(error_text));

    emit_event(*navigable, "network.fetchError"_string, move(params));
}

// Blocks the request at the phase if an intercept matches it, and answers the interception at once otherwise. Returns
// the intercepts that blocked the request.
static JsonArray block_network_request_if_intercepted(ObservedRequestSource const& source, Fetch::Engine::Event::ObservedRequest const& request, Fetch::Engine::NetworkPhase phase, Fetch::Engine::ContinuationId interception)
{
    auto& controller = source.controller();
    auto intercepts = g_network_events_enabled ? get_network_intercepts(source, request, phase_name(phase)) : JsonArray {};
    auto state = source.network_request();
    // NB: A request has one block at a time, so a fetch whose request another fetch holds blocked goes on.
    if (intercepts.is_empty() || blocked_requests().contains(state->id)) {
        controller.resolve_network_interception(interception, Fetch::Engine::ContinueFetch {});
        return {};
    }
    blocked_requests().set(state->id, BlockedRequest { controller.registry(), controller.token(), source.cors_preflight_id(), interception, phase });
    return intercepts;
}

void receive_network_activity(FetchController& controller, Fetch::Engine::Event::FetchNetworkActivity const& activity)
{
    ObservedRequestSource source { controller, activity.cors_preflight_id };
    auto const& request = activity.request;
    auto timing_info = timing_info_of(request);
    activity.activity.visit(
        [&](Fetch::Engine::Event::FetchNetworkActivity::BeforeRequestSent const& reached) {
            JsonArray intercepts;
            if (reached.interception.has_value())
                intercepts = block_network_request_if_intercepted(source, request, Fetch::Engine::NetworkPhase::BeforeRequestSent, *reached.interception);
            if (g_network_events_enabled)
                network_before_request_sent(source, request, timing_info, intercepts);
        },
        [&](Fetch::Engine::Event::FetchNetworkActivity::ResponseStarted const& reached) {
            JsonArray intercepts;
            if (reached.interception.has_value())
                intercepts = block_network_request_if_intercepted(source, request, Fetch::Engine::NetworkPhase::ResponseStarted, *reached.interception);
            if (g_network_events_enabled)
                network_response_started(source, request, reached.response, timing_info, intercepts);
        },
        [&](Fetch::Engine::Event::FetchNetworkActivity::AuthRequired const& reached) {
            JsonArray intercepts;
            if (reached.interception.has_value())
                intercepts = block_network_request_if_intercepted(source, request, Fetch::Engine::NetworkPhase::AuthRequired, *reached.interception);
            if (g_network_events_enabled)
                network_auth_required(source, request, reached.response, timing_info, intercepts);
        },
        [&](Fetch::Engine::Event::FetchNetworkActivity::RequestUpdated const&) {
            if (auto state = source.network_request())
                state->request_data.set(request.redirect_count, get_the_request_data(*state, request, timing_info, source.end_time()));
        },
        [&](Fetch::Engine::Event::FetchNetworkActivity::ResponseCompleted const& completed) {
            if (g_network_events_enabled)
                network_response_completed(source, request, completed.response, timing_info);
        },
        [&](Fetch::Engine::Event::FetchNetworkActivity::Failed const& failed) {
            if (g_network_events_enabled)
                network_fetch_error(source, request, timing_info, failed.reason);
        });
}

void network_response_completed(FetchController& controller, Fetch::Engine::Event::ObservedResponse const& response, FetchTimingInfo const& timing_info)
{
    auto const& request = controller.observed_request();
    if (!request.has_value())
        return;
    network_response_completed(ObservedRequestSource { controller, {} }, *request, response, timing_info);
}

void network_fetch_stopped(FetchController& controller, StringView error_text)
{
    // A stopped fetch releases its interceptions even if its request is already done.
    forget_blocked_requests(controller);
    auto const& request = controller.observed_request();
    if (!g_network_events_enabled || !request.has_value() || controller.request()->done())
        return;
    auto timing_info = controller.control_block().timing_info_with(request->timing_seed);
    network_fetch_error(ObservedRequestSource { controller, {} }, *request, timing_info, error_text);
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
    auto phase = phase_name(it->value.phase);
    if ((method == "network.continueRequest"sv && phase != "beforeRequestSent"sv)
        || (method == "network.continueResponse"sv && phase == "beforeRequestSent"sv)
        || (method == "network.continueWithAuth"sv && phase != "authRequired"sv)
        || (method == "network.failRequest"sv && phase == "authRequired"sv))
        return Error::from_code(ErrorCode::InvalidArgument, "Command does not apply to the blocked phase"sv);
    auto overrides = TRY(Network::deserialize_interception_overrides(method, params));
    Fetch::Engine::NetworkDecision decision = Fetch::Engine::ContinueFetch {};
    if (method == "network.continueRequest"sv) {
        // AD-HOC: The intercepted request goes on to HTTP-network fetch, so it keeps an HTTP(S) URL.
        if (overrides.url.has_value() && !Fetch::Infrastructure::is_http_or_https_scheme(overrides.url->scheme()))
            return Error::from_code(ErrorCode::InvalidArgument, "An intercepted HTTP request requires an HTTP(S) URL"sv);

        if (params.has("url"sv) || params.has("method"sv) || params.has("headers"sv) || params.has("body"sv) || params.has("cookies"sv))
            decision = Fetch::Engine::ModifyRequest { move(overrides.url), move(overrides.method), move(overrides.headers), move(overrides.cookie_header), move(overrides.body) };
    } else if (method == "network.failRequest"sv) {
        decision = Fetch::Engine::FailFetch {};
    } else {
        Fetch::Engine::ModifyResponse response {
            .status = overrides.status.map([](u64 status) { return static_cast<u16>(status); }),
            .reason = move(overrides.reason),
            .headers = move(overrides.headers),
            .set_cookie_values = move(overrides.set_cookie_values),
        };
        auto credentials = params.get_object("credentials"sv);
        auto provides_credentials = phase == "authRequired"sv && credentials.has_value() && (method == "network.continueResponse"sv || params.get_string("action"sv) == "provideCredentials"sv);
        if (method == "network.provideResponse"sv) {
            decision = Fetch::Engine::ProvideResponse { move(response), move(overrides.body) };
        } else if (provides_credentials) {
            auto payload = MUST(String::formatted("{}:{}", *credentials->get_string("username"sv), *credentials->get_string("password"sv)));
            decision = Fetch::Engine::Authenticate { ByteString::formatted("Basic {}", MUST(encode_base64(payload.bytes()))), move(response) };
        } else if (method == "network.continueWithAuth"sv && params.get_string("action"sv) == "cancel"sv) {
            decision = Fetch::Engine::CancelAuthentication {};
        } else if (method == "network.continueResponse"sv) {
            decision = move(response);
        }
    }
    resume_blocked_request(id, move(decision));
    return JsonValue {};
}

WebDriver::Response continue_network_request(StringView method, JsonObject const& params)
{
    auto result = continue_network_request_impl(method, params);
    if (result.is_error())
        return result.release_error();
    return result.release_value();
}

Optional<JsonObject> network_response_for_navigation(Request const& request, FetchTimingInfo const& timing_info,
    Response const& response)
{
    if (!g_network_events_enabled)
        return {};
    auto const& observation = request.network_observation();
    auto state = observation ? static_cast<NetworkRequest*>(observation->observer_state().ptr()) : nullptr;
    if (!state || state->completed.contains(request.redirect_count()))
        return {};
    auto navigable = navigable_of(*state);
    if (!navigable)
        return {};
    state->transferred.set(request.redirect_count());
    JsonObject result;
    result.set("parameters"sv, process_a_network_event(*navigable, *state, Fetch::Fetching::observed_request(request, timing_info), timing_info, timing_info.end_time()));
    result.set("response"sv, get_the_response_data(Fetch::Fetching::observed_response(response), timing_info));
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
            request->set("timings"sv, get_the_fetch_timings(timings, timings.end_time(), origin->is_null() ? Optional<double> {} : origin->get_double_with_precision_loss(), epoch_origin));
        }
        params.set("response"sv, get_the_response_data(Fetch::Fetching::observed_response(response), timings));
    }
    emit_event(navigable, MUST(String::from_utf8(name)), move(params));
}

}
