/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Fetching {

// AD-HOC: A header list a network interception gives may echo headers that Fetch generates. Its values for those
//         replace the ones Fetch generates for the request again.
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

static bool is_fetch_generated_request_header_name(StringView name)
{
    return any_of(fetch_generated_request_header_names, [&](auto generated_name) { return name.equals_ignoring_ascii_case(generated_name); });
}

Engine::Event::ObservedRequest observed_request(Infrastructure::Request const& request, Infrastructure::FetchTimingInfo const& timing_info)
{
    Optional<u64> body_size;
    if (auto const* bytes = request.body().get_pointer<ByteBuffer>())
        body_size = bytes->size();
    else if (auto const* body = request.body().get_pointer<Infrastructure::FetchBody>())
        body_size = body->length();

    auto const& client = request.client_snapshot();
    auto const* traversable_for_user_prompts = request.traversable_for_user_prompts().get_pointer<HTML::CrossProcessId>();
    Optional<HTML::CrossProcessId> navigable_id;
    if (auto const& navigation = request.observed_navigation(); navigation.has_value())
        navigable_id = navigation->navigable_id;
    else if (client && client->navigable_id.has_value())
        navigable_id = client->navigable_id;
    else if (traversable_for_user_prompts)
        navigable_id = *traversable_for_user_prompts;

    String destination;
    if (request.destination().has_value())
        destination = MUST(String::from_utf8(Infrastructure::request_destination_to_string(*request.destination())));

    Optional<String> initiator_type;
    if (request.initiator_type().has_value())
        initiator_type = Infrastructure::initiator_type_to_string(*request.initiator_type()).to_utf16_string().to_utf8();

    return {
        .url = request.current_url(),
        .method = request.method(),
        .headers = request.header_list()->headers(),
        .body_size = body_size,
        .destination = move(destination),
        .initiator_type = move(initiator_type),
        .redirect_count = request.redirect_count(),
        .navigable_id = navigable_id,
        .navigation_id = request.observed_navigation().has_value() ? request.observed_navigation()->navigation_id : Optional<String> {},
        .time_origin = client ? Optional<double> { client->time_origin } : Optional<double> {},
        .timing_seed = timing_info.seed(),
        .network_phases = timing_info.network_phases(),
    };
}

Engine::Event::ObservedResponse observed_response(Infrastructure::Response const& response)
{
    auto url = response.url();
    auto body_info = response.body_info();
    if (auto const& body = response.body(); body.has_value() && body->source().has_value() && body_info.decoded_size == 0) {
        body_info.decoded_size = body->source()->size();
        if (url.has_value() && url->scheme() == "data"sv)
            body_info.encoded_size = body_info.decoded_size;
    }

    ByteString protocol;
    if (auto const& network_transport = response.network_transport(); network_transport.has_value())
        protocol = network_transport->handle->http_version();

    return {
        .url = url.map([](auto const& value) { return value; }),
        .status = response.status(),
        .status_message = response.status_message(),
        .headers = response.header_list()->headers(),
        .protocol = move(protocol),
        .from_cache = response.cache_state() == Infrastructure::Response::CacheState::Local,
        .encoded_body_size = body_info.encoded_size,
        .decoded_body_size = body_info.decoded_size,
    };
}

static Engine::Event::FetchNetworkActivity network_activity(Infrastructure::FetchParams const& fetch_params, Engine::Event::FetchNetworkActivity::Activity activity)
{
    return {
        .cors_preflight_id = fetch_params.cors_preflight_id(),
        .request = observed_request(fetch_params.request(), fetch_params.timing_info()),
        .activity = move(activity),
    };
}

static bool observes_fetches(Infrastructure::FetchParams const& fetch_params)
{
    auto const* engine_record = fetch_params.engine_record();
    VERIFY(engine_record);
    return engine_record->network_activity_observers().observes_fetches();
}

void report_response_started(Infrastructure::FetchParams const& fetch_params, Infrastructure::Response const& response)
{
    if (!observes_fetches(fetch_params))
        return;
    Engine::Event::FetchNetworkActivity::ResponseStarted started { .response = observed_response(response), .interception = {} };
    fetch_params.engine_record()->report_network_activity(network_activity(fetch_params, move(started)));
}

void report_response_completed(Infrastructure::FetchParams const& fetch_params, Infrastructure::Response const& response)
{
    if (!observes_fetches(fetch_params))
        return;
    Engine::Event::FetchNetworkActivity::ResponseCompleted completed { observed_response(response) };
    fetch_params.engine_record()->report_network_activity(network_activity(fetch_params, move(completed)));
}

void report_fetch_error(Infrastructure::FetchParams const& fetch_params, String reason)
{
    if (!observes_fetches(fetch_params))
        return;
    Engine::Event::FetchNetworkActivity::Failed failed { move(reason) };
    fetch_params.engine_record()->report_network_activity(network_activity(fetch_params, move(failed)));
}

static Engine::Event::FetchNetworkActivity::Activity network_phase_activity(Engine::NetworkPhase phase, Infrastructure::Response const* response)
{
    switch (phase) {
    case Engine::NetworkPhase::BeforeRequestSent:
        return Engine::Event::FetchNetworkActivity::BeforeRequestSent {};
    case Engine::NetworkPhase::ResponseStarted:
        return Engine::Event::FetchNetworkActivity::ResponseStarted { .response = observed_response(*response), .interception = {} };
    case Engine::NetworkPhase::AuthRequired:
        return Engine::Event::FetchNetworkActivity::AuthRequired { .response = observed_response(*response), .interception = {} };
    }
    VERIFY_NOT_REACHED();
}

static void modify_request(Infrastructure::Request& request, Infrastructure::Request* original_request, Engine::ModifyRequest overrides)
{
    auto& request_with_header_overrides = original_request ? *original_request : request;
    if (overrides.headers.has_value()) {
        auto generated_header_overrides = HTTP::HeaderList::create();
        for (auto const& header : *overrides.headers) {
            if (is_fetch_generated_request_header_name(header.name))
                generated_header_overrides->append(header);
        }
        request_with_header_overrides.set_generated_header_overrides(move(generated_header_overrides));
    }

    if (original_request && original_request != &request) {
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
        if (overrides.cookie.has_value())
            original_request->header_list()->set({ "Cookie"sv, *overrides.cookie });
    }

    if (overrides.url.has_value())
        request.url_list().last() = overrides.url.release_value();
    if (overrides.method.has_value())
        request.set_method(overrides.method.release_value());
    if (overrides.headers.has_value())
        request.set_header_list(HTTP::HeaderList::create(overrides.headers.release_value()));
    if (overrides.cookie.has_value())
        request.header_list()->set({ "Cookie"sv, overrides.cookie.release_value() });

    if (overrides.body.has_value()) {
        remove_content_length_override(request_with_header_overrides);
        if (original_request)
            original_request->header_list()->delete_("Content-Length"sv);
        auto body = Infrastructure::FetchBody::from_bytes(Core::ImmutableBytes::adopt(overrides.body.release_value()));
        request.set_body(body);
        if (original_request && original_request != &request)
            original_request->set_body(body);
        request.header_list()->set({ "Content-Length"sv, ByteString::number(*body.length()) });
    }
}

// https://w3c.github.io/webdriver-bidi/#update-the-response
static void modify_response(Infrastructure::Response& response, Engine::ModifyResponse overrides)
{
    // 7. If command parameters contains "statusCode":
    if (overrides.status.has_value())
        response.set_status(*overrides.status);

    // 8. If command parameters contains "reasonPhrase":
    if (overrides.reason.has_value())
        response.set_status_message(overrides.reason.release_value());

    // 9. If command parameters contains "headers":
    auto has_headers = overrides.headers.has_value();
    if (has_headers)
        response.set_header_list(HTTP::HeaderList::create(overrides.headers.release_value()));

    // 10. If command parameters contains "cookies":
    if (overrides.set_cookie_values.has_value()) {
        if (!has_headers) {
            response.set_header_list(HTTP::HeaderList::create(response.header_list()->headers()));
            response.header_list()->delete_("Set-Cookie"sv);
        }
        for (auto& value : *overrides.set_cookie_values)
            response.header_list()->append({ "Set-Cookie"sv, move(value) });
    }
}

static NonnullRefPtr<Infrastructure::Response> provide_response(Infrastructure::FetchParams const& fetch_params, RefPtr<Infrastructure::Response> response, Engine::ProvideResponse provided)
{
    auto original_response = response;
    if (!response) {
        response = Infrastructure::Response::create();
        response->set_url_list(fetch_params.request()->url_list());
    }
    modify_response(*response, move(provided.response));
    if (!provided.body.has_value())
        return response.release_nonnull();

    // NB: A replacement body has no RequestServer transport to transfer to a new navigation process.
    if (original_response)
        fetch_params.discard_response_body(*original_response);
    auto replacement = Infrastructure::Response::create();
    replacement->set_url_list(response->url_list());
    replacement->set_status(response->status());
    replacement->set_status_message(response->status_message());
    replacement->set_header_list(response->header_list());
    auto body = Infrastructure::ResponseBody::from_bytes(Core::ImmutableBytes::adopt(provided.body.release_value()));
    auto length = *body.length();
    replacement->set_body(move(body));
    auto body_info = replacement->body_info();
    body_info.encoded_size = length;
    body_info.decoded_size = length;
    replacement->set_body_info(body_info);
    return replacement;
}

bool can_wait_at_network_phase(Infrastructure::FetchParams const& fetch_params, Engine::NetworkPhase phase)
{
    auto const* engine_record = fetch_params.engine_record();
    VERIFY(engine_record);
    return engine_record->can_wait_at_network_phase(phase);
}

void report_network_phase(Infrastructure::FetchParams const& fetch_params, Engine::NetworkPhase phase, Infrastructure::Response const* response)
{
    if (!observes_fetches(fetch_params))
        return;
    fetch_params.engine_record()->report_network_activity(network_activity(fetch_params, network_phase_activity(phase, response)));
}

void wait_at_network_phase(Infrastructure::FetchParams const& fetch_params, Engine::NetworkPhase phase, RefPtr<Infrastructure::Response> response, RefPtr<Infrastructure::Request> original_request, Function<void(Engine::NetworkDecision const&, RefPtr<Infrastructure::Response>)> continuation)
{
    auto activity = network_activity(fetch_params, network_phase_activity(phase, response.ptr()));
    auto apply_decision = [fetch_params = NonnullRefPtr { fetch_params }, response, original_request = move(original_request), continuation = move(continuation)](Engine::NetworkDecision decision) mutable {
        auto continued_response = decision.visit(
            [&](Engine::ContinueFetch const&) { return response; },
            [&](Engine::FailFetch const&) -> RefPtr<Infrastructure::Response> {
                if (response)
                    fetch_params->discard_response_body(*response);
                if (fetch_params->engine_record()->is_stopped())
                    return Infrastructure::Response::network_error("Intercepted fetch was stopped"_string);
                return Infrastructure::Response::network_error("Request failed by WebDriver BiDi"_string);
            },
            [&](Engine::ModifyRequest& overrides) -> RefPtr<Infrastructure::Response> {
                modify_request(fetch_params->request(), original_request.ptr(), move(overrides));
                if (observes_fetches(*fetch_params))
                    fetch_params->engine_record()->report_network_activity(network_activity(*fetch_params, Engine::Event::FetchNetworkActivity::RequestUpdated {}));
                return nullptr;
            },
            [&](Engine::ModifyResponse& overrides) {
                modify_response(*response, move(overrides));
                return response;
            },
            [&](Engine::ProvideResponse& provided) -> RefPtr<Infrastructure::Response> {
                return provide_response(*fetch_params, response, move(provided));
            },
            [&](Engine::Authenticate& authenticate) {
                modify_response(*response, move(authenticate.response));
                fetch_params->request()->header_list()->set({ "Authorization"sv, move(authenticate.authorization) });
                return response;
            },
            [&](Engine::CancelAuthentication const&) { return response; });
        continuation(decision, move(continued_response));
    };
    fetch_params.engine_record()->wait_at_network_phase(move(activity), move(apply_decision));
}

static Optional<HTML::CrossProcessId> top_level_traversable_for_network_configuration(Infrastructure::Request const& request)
{
    auto const* traversable_for_user_prompts = request.traversable_for_user_prompts().get_pointer<HTML::CrossProcessId>();
    // AD-HOC: For a navigation request, use the target navigable's top-level traversable.
    if (request.observed_navigation().has_value() && traversable_for_user_prompts)
        return *traversable_for_user_prompts;
    if (auto const& client = request.client_snapshot(); client && client->top_level_traversable_id.has_value())
        return client->top_level_traversable_id;
    if (traversable_for_user_prompts)
        return *traversable_for_user_prompts;
    return {};
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-cache-behavior
static bool webdriver_bidi_cache_behavior_is_bypass(Engine::NetworkConfiguration const& configuration, Infrastructure::Request const& request)
{
    // 1. Let navigable be null.
    // 2. If request's client is an environment settings object:
    //     1. Let environment settings be request's client.
    //     2. If there is a navigable whose active window is environment settings' global object, set navigable to
    //        that navigable's top-level traversable.
    auto navigable = top_level_traversable_for_network_configuration(request);

    // 3. If navigable is not null and navigable cache behavior map contains navigable, return navigable cache behavior
    //    map[navigable].
    if (navigable.has_value()) {
        if (auto bypass = configuration.bypass_cache_by_traversable.get(Engine::NetworkConfiguration::traversable_name(*navigable)); bypass.has_value())
            return *bypass;
    }

    // 4. Return default cache behavior.
    return configuration.bypass_cache_by_default;
}

bool webdriver_bidi_bypasses_cache(Infrastructure::FetchParams const& fetch_params)
{
    auto const* engine_record = fetch_params.engine_record();
    VERIFY(engine_record);
    auto configuration = engine_record->network_activity_observers().network_configuration();
    return configuration && webdriver_bidi_cache_behavior_is_bypass(*configuration, fetch_params.request());
}

// https://w3c.github.io/webdriver-bidi/#update-headers
static void update_headers(Infrastructure::Request& request, Vector<HTTP::Header> const& headers)
{
    // 1. Let request headers be request's header list.
    auto request_headers = request.header_list();

    // 2. For each header in headers:
    for (auto const& header : headers) {
        // 1. Set header in request headers.
        // NOTE: This always overwrites the existing value, if any. In particular it doesn't append cookies to an
        //       existing `Set-Cookie` header.
        request_headers->set(header);
    }
}

// https://w3c.github.io/webdriver-bidi/#update-request-headers
void update_request_headers_for_webdriver_bidi(Infrastructure::FetchParams const& fetch_params)
{
    auto const* engine_record = fetch_params.engine_record();
    VERIFY(engine_record);
    auto configuration = engine_record->network_activity_observers().network_configuration();
    if (!configuration)
        return;
    auto request = fetch_params.request();

    auto top_level_traversable = top_level_traversable_for_network_configuration(request);

    // 1. Assert: related navigables's size is 0 or 1.
    // 2. Update headers with request and session's extra headers' default headers
    update_headers(request, configuration->default_headers);

    // 3. Let user context headers be session's extra headers' user context headers.
    // 4. For navigable in related navigables:
    if (top_level_traversable.has_value()) {
        // 1. Let user context be navigable's associated user context.
        // 2. If user context headers contains user context then update headers with request and user context
        //    headers[user context]
        update_headers(request, configuration->default_user_context_headers);
    }

    // 5. Let navigable headers be session's extra headers' navigable headers.
    // 6. For navigable in related navigables:
    if (top_level_traversable.has_value()) {
        // 1. Let top-level traversable be navigable's top-level traversable.
        // 2. If navigable headers contains top-level traversable update headers with request and navigable
        //    headers[top-level traversable].
        if (auto headers = configuration->traversable_headers.get(Engine::NetworkConfiguration::traversable_name(*top_level_traversable)); headers.has_value())
            update_headers(request, *headers);
    }
}

void apply_generated_header_overrides(Infrastructure::Request& request)
{
    auto const& overrides = request.generated_header_overrides();
    if (!overrides)
        return;
    for (auto name : fetch_generated_request_header_names) {
        if (!overrides->contains(name))
            continue;
        request.header_list()->delete_(name);
        for (auto const& header : *overrides) {
            if (header.name.equals_ignoring_ascii_case(name))
                request.header_list()->append(header);
        }
    }
}

void remove_content_length_override(Infrastructure::Request& request)
{
    auto const& overrides = request.generated_header_overrides();
    if (!overrides)
        return;
    request.header_list()->delete_("Content-Length"sv);
    auto remaining_overrides = HTTP::HeaderList::create(overrides->headers());
    remaining_overrides->delete_("Content-Length"sv);
    request.set_generated_header_overrides(move(remaining_overrides));
}

}
