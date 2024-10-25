/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Debug.h>
#include <AK/ScopeGuard.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Fetching/Checks.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Fetching/RefCountedFlag.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Methods.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/Fetch/Infrastructure/MimeTypeBlocking.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Fetch/Infrastructure/NoSniffBlocking.h>
#include <LibWeb/Fetch/Infrastructure/PortBlocking.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/LoadRequest.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MixedContent/AbstractOperations.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>
#include <LibWeb/SRI/SRI.h>
#include <LibWeb/SecureContexts/AbstractOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/TransformStreamDefaultController.h>
#include <LibWeb/Streams/Transformer.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Fetch::Fetching {

bool g_http_cache_enabled;

#define TRY_OR_IGNORE(expression)                                                                    \
    ({                                                                                               \
        auto&& _temporary_result = (expression);                                                     \
        if (_temporary_result.is_error())                                                            \
            return;                                                                                  \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        _temporary_result.release_value();                                                           \
    })

// https://fetch.spec.whatwg.org/#concept-fetch
WebIDL::ExceptionOr<JS::NonnullGCPtr<Infrastructure::FetchController>> fetch(JS::Realm& realm, Infrastructure::Request& request, Infrastructure::FetchAlgorithms const& algorithms, UseParallelQueue use_parallel_queue)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'fetch' with: request @ {}", &request);

    auto& vm = realm.vm();

    // 1. Assert: request’s mode is "navigate" or processEarlyHintsResponse is null.
    VERIFY(request.mode() == Infrastructure::Request::Mode::Navigate || !algorithms.process_early_hints_response());

    // 2. Let taskDestination be null.
    JS::GCPtr<JS::Object> task_destination;

    // 3. Let crossOriginIsolatedCapability be false.
    auto cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No;

    // 4. If request’s client is non-null, then:
    if (request.client() != nullptr) {
        // 1. Set taskDestination to request’s client’s global object.
        task_destination = request.client()->global_object();

        // 2. Set crossOriginIsolatedCapability to request’s client’s cross-origin isolated capability.
        cross_origin_isolated_capability = request.client()->cross_origin_isolated_capability();
    }

    // FIXME: 5. If useParallelQueue is true, then set taskDestination to the result of starting a new parallel queue.
    (void)use_parallel_queue;

    // 6. Let timingInfo be a new fetch timing info whose start time and post-redirect start time are the coarsened
    //    shared current time given crossOriginIsolatedCapability, and render-blocking is set to request’s
    //    render-blocking.
    auto timing_info = Infrastructure::FetchTimingInfo::create(vm);
    auto now = HighResolutionTime::coarsened_shared_current_time(cross_origin_isolated_capability == HTML::CanUseCrossOriginIsolatedAPIs::Yes);
    timing_info->set_start_time(now);
    timing_info->set_post_redirect_start_time(now);
    timing_info->set_render_blocking(request.render_blocking());

    // 7. Let fetchParams be a new fetch params whose request is request, timing info is timingInfo, process request
    //    body chunk length is processRequestBodyChunkLength, process request end-of-body is processRequestEndOfBody,
    //    process early hints response is processEarlyHintsResponse, process response is processResponse, process
    //    response consume body is processResponseConsumeBody, process response end-of-body is processResponseEndOfBody,
    //    task destination is taskDestination, and cross-origin isolated capability is crossOriginIsolatedCapability.
    auto fetch_params = Infrastructure::FetchParams::create(vm, request, timing_info);
    fetch_params->set_algorithms(algorithms);
    if (task_destination)
        fetch_params->set_task_destination({ *task_destination });
    fetch_params->set_cross_origin_isolated_capability(cross_origin_isolated_capability);

    // 8. If request’s body is a byte sequence, then set request’s body to request’s body as a body.
    if (auto const* buffer = request.body().get_pointer<ByteBuffer>())
        request.set_body(TRY(Infrastructure::byte_sequence_as_body(realm, buffer->bytes())));

    // 9. If request’s window is "client", then set request’s window to request’s client, if request’s client’s global
    //    object is a Window object; otherwise "no-window".
    auto const* window = request.window().get_pointer<Infrastructure::Request::Window>();
    if (window && *window == Infrastructure::Request::Window::Client) {
        if (is<HTML::Window>(request.client()->global_object())) {
            request.set_window(request.client());
        } else {
            request.set_window(Infrastructure::Request::Window::NoWindow);
        }
    }

    // 10. If request’s origin is "client", then set request’s origin to request’s client’s origin.
    auto const* origin = request.origin().get_pointer<Infrastructure::Request::Origin>();
    if (origin && *origin == Infrastructure::Request::Origin::Client)
        request.set_origin(request.client()->origin());

    // 11. If all of the following conditions are true:
    if (
        // - request’s URL’s scheme is an HTTP(S) scheme
        Infrastructure::is_http_or_https_scheme(request.url().scheme())
        // - request’s mode is "same-origin", "cors", or "no-cors"
        && (request.mode() == Infrastructure::Request::Mode::SameOrigin || request.mode() == Infrastructure::Request::Mode::CORS || request.mode() == Infrastructure::Request::Mode::NoCORS)
        // - request’s window is an environment settings object
        && request.window().has<JS::GCPtr<HTML::EnvironmentSettingsObject>>()
        // - request’s method is `GET`
        && StringView { request.method() }.equals_ignoring_ascii_case("GET"sv)
        // - request’s unsafe-request flag is not set or request’s header list is empty
        && (!request.unsafe_request() || request.header_list()->is_empty())) {
        // 1. Assert: request’s origin is same origin with request’s client’s origin.
        VERIFY(request.origin().has<URL::Origin>() && request.origin().get<URL::Origin>().is_same_origin(request.client()->origin()));

        // 2. Let onPreloadedResponseAvailable be an algorithm that runs the following step given a response
        //    response: set fetchParams’s preloaded response candidate to response.
        auto on_preloaded_response_available = JS::create_heap_function(realm.heap(), [fetch_params](JS::NonnullGCPtr<Infrastructure::Response> response) {
            fetch_params->set_preloaded_response_candidate(response);
        });

        // FIXME: 3. Let foundPreloadedResource be the result of invoking consume a preloaded resource for request’s
        //    window, given request’s URL, request’s destination, request’s mode, request’s credentials mode,
        //    request’s integrity metadata, and onPreloadedResponseAvailable.
        auto found_preloaded_resource = false;
        (void)on_preloaded_response_available;

        // 4. If foundPreloadedResource is true and fetchParams’s preloaded response candidate is null, then set
        //    fetchParams’s preloaded response candidate to "pending".
        if (found_preloaded_resource && fetch_params->preloaded_response_candidate().has<Empty>())
            fetch_params->set_preloaded_response_candidate(Infrastructure::FetchParams::PreloadedResponseCandidatePendingTag {});
    }

    // 12. If request’s policy container is "client", then:
    auto const* policy_container = request.policy_container().get_pointer<Infrastructure::Request::PolicyContainer>();
    if (policy_container) {
        VERIFY(*policy_container == Infrastructure::Request::PolicyContainer::Client);
        // 1. If request’s client is non-null, then set request’s policy container to a clone of request’s client’s
        //    policy container.
        if (request.client() != nullptr)
            request.set_policy_container(request.client()->policy_container());
        // 2. Otherwise, set request’s policy container to a new policy container.
        else
            request.set_policy_container(HTML::PolicyContainer {});
    }

    // 13. If request’s header list does not contain `Accept`, then:
    if (!request.header_list()->contains("Accept"sv.bytes())) {
        // 1. Let value be `*/*`.
        auto value = "*/*"sv;

        // 2. If request’s initiator is "prefetch", then set value to the document `Accept` header value.
        if (request.initiator() == Infrastructure::Request::Initiator::Prefetch) {
            value = document_accept_header_value;
        }

        // 3. Otherwise, the user agent should set value to the first matching statement, if any, switching on request’s destination:
        else if (request.destination().has_value()) {
            switch (*request.destination()) {
            // -> "document"
            // -> "frame"
            // -> "iframe"
            case Infrastructure::Request::Destination::Document:
            case Infrastructure::Request::Destination::Frame:
            case Infrastructure::Request::Destination::IFrame:
                // the document `Accept` header value
                value = document_accept_header_value;
                break;
            // -> "image"
            case Infrastructure::Request::Destination::Image:
                // `image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5`
                value = "image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5"sv;
                break;
            // -> "json"
            case Infrastructure::Request::Destination::JSON:
                // `application/json,*/*;q=0.5`
                value = "application/json,*/*;q=0.5"sv;
                break;
            // -> "style"
            case Infrastructure::Request::Destination::Style:
                // `text/css,*/*;q=0.1`
                value = "text/css,*/*;q=0.1"sv;
                break;
            default:
                break;
            }
        }

        // 4. Append (`Accept`, value) to request’s header list.
        auto header = Infrastructure::Header::from_string_pair("Accept"sv, value.bytes());
        request.header_list()->append(move(header));
    }

    // 14. If request’s header list does not contain `Accept-Language`, then user agents should append
    //     (`Accept-Language, an appropriate header value) to request’s header list.
    if (!request.header_list()->contains("Accept-Language"sv.bytes())) {
        StringBuilder accept_language;
        accept_language.join(","sv, ResourceLoader::the().preferred_languages());

        auto header = Infrastructure::Header::from_string_pair("Accept-Language"sv, accept_language.string_view());
        request.header_list()->append(move(header));
    }

    // 15. If request’s priority is null, then use request’s initiator, destination, and render-blocking appropriately
    //     in setting request’s priority to a user-agent-defined object.
    // NOTE: The user-agent-defined object could encompass stream weight and dependency for HTTP/2, and equivalent
    //       information used to prioritize dispatch and processing of HTTP/1 fetches.

    // 16. If request is a subresource request, then:
    if (request.is_subresource_request()) {
        // 1. Let record be a new fetch record whose request is request and controller is fetchParams’s controller.
        auto record = Infrastructure::FetchRecord::create(vm, request, fetch_params->controller());

        // 2. Append record to request’s client’s fetch group list of fetch records.
        request.client()->fetch_group().append(record);
    }

    // 17. Run main fetch given fetchParams.
    (void)TRY(main_fetch(realm, fetch_params));

    // 18. Return fetchParams’s controller.
    return fetch_params->controller();
}

// https://fetch.spec.whatwg.org/#concept-main-fetch
WebIDL::ExceptionOr<JS::GCPtr<PendingResponse>> main_fetch(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params, Recursive recursive)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'main fetch' with: fetch_params @ {}", &fetch_params);

    auto& vm = realm.vm();

    // 1. Let request be fetchParams’s request.
    auto request = fetch_params.request();

    // 2. Let response be null.
    JS::GCPtr<Infrastructure::Response> response;

    // 3. If request’s local-URLs-only flag is set and request’s current URL is not local, then set response to a
    //    network error.
    if (request->local_urls_only() && !Infrastructure::is_local_url(request->current_url()))
        response = Infrastructure::Response::network_error(vm, "Request with 'local-URLs-only' flag must have a local URL"sv);

    // FIXME: 4. Run report Content Security Policy violations for request.
    // FIXME: 5. Upgrade request to a potentially trustworthy URL, if appropriate.

    // 6. Upgrade a mixed content request to a potentially trustworthy URL, if appropriate.
    MixedContent::upgrade_a_mixed_content_request_to_a_potentially_trustworthy_url_if_appropriate(request);

    // 7. If should request be blocked due to a bad port, should fetching request be blocked as mixed content, or
    //    should request be blocked by Content Security Policy returns blocked, then set response to a network error.
    if (Infrastructure::block_bad_port(request) == Infrastructure::RequestOrResponseBlocking::Blocked
        || MixedContent::should_fetching_request_be_blocked_as_mixed_content(request) == Infrastructure::RequestOrResponseBlocking::Blocked
        || false // FIXME: "should request be blocked by Content Security Policy returns blocked"
    ) {
        response = Infrastructure::Response::network_error(vm, "Request was blocked"sv);
    }

    // 8. If request’s referrer policy is the empty string, then set request’s referrer policy to request’s policy
    //    container’s referrer policy.
    if (request->referrer_policy() == ReferrerPolicy::ReferrerPolicy::EmptyString) {
        VERIFY(request->policy_container().has<HTML::PolicyContainer>());
        request->set_referrer_policy(request->policy_container().get<HTML::PolicyContainer>().referrer_policy);
    }

    // 9. If request’s referrer is not "no-referrer", then set request’s referrer to the result of invoking determine
    //    request’s referrer.
    // NOTE: As stated in Referrer Policy, user agents can provide the end user with options to override request’s
    //       referrer to "no-referrer" or have it expose less sensitive information.
    auto const* referrer = request->referrer().get_pointer<Infrastructure::Request::Referrer>();
    if (!referrer || *referrer != Infrastructure::Request::Referrer::NoReferrer) {
        auto determined_referrer = ReferrerPolicy::determine_requests_referrer(request);
        if (determined_referrer.has_value())
            request->set_referrer(*determined_referrer);
        else
            request->set_referrer(Infrastructure::Request::Referrer::NoReferrer);
    }

    // 10. Set request’s current URL’s scheme to "https" if all of the following conditions are true:
    if (
        // - request’s current URL’s scheme is "http"
        request->current_url().scheme() == "http"sv
        // - request’s current URL’s host is a domain
        && DOMURL::host_is_domain(request->current_url().host())
        // FIXME: - Matching request’s current URL’s host per Known HSTS Host Domain Name Matching results in either a
        //          superdomain match with an asserted includeSubDomains directive or a congruent match (with or without an
        //          asserted includeSubDomains directive) [HSTS]; or DNS resolution for the request finds a matching HTTPS RR
        //          per section 9.5 of [SVCB].
        && false

    ) {
        request->current_url().set_scheme("https"_string);
    }

    JS::SafeFunction<WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>>()> get_response = [&realm, &vm, &fetch_params, request]() -> WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> {
        dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'main fetch' get_response() function");

        // -> fetchParams’s preloaded response candidate is not null
        if (!fetch_params.preloaded_response_candidate().has<Empty>()) {
            // 1. Wait until fetchParams’s preloaded response candidate is not "pending".
            HTML::main_thread_event_loop().spin_until([&] {
                return !fetch_params.preloaded_response_candidate().has<Infrastructure::FetchParams::PreloadedResponseCandidatePendingTag>();
            });

            // 2. Assert: fetchParams’s preloaded response candidate is a response.
            VERIFY(fetch_params.preloaded_response_candidate().has<JS::NonnullGCPtr<Infrastructure::Response>>());

            // 3. Return fetchParams’s preloaded response candidate.
            return PendingResponse::create(vm, request, fetch_params.preloaded_response_candidate().get<JS::NonnullGCPtr<Infrastructure::Response>>());
        }
        // -> request’s current URL’s origin is same origin with request’s origin, and request’s response tainting
        //    is "basic"
        // -> request’s current URL’s scheme is "data"
        // -> request’s mode is "navigate" or "websocket"
        else if (
            (request->origin().has<URL::Origin>() && request->current_url().origin().is_same_origin(request->origin().get<URL::Origin>()) && request->response_tainting() == Infrastructure::Request::ResponseTainting::Basic)
            || request->current_url().scheme() == "data"sv
            || (request->mode() == Infrastructure::Request::Mode::Navigate || request->mode() == Infrastructure::Request::Mode::WebSocket)) {
            // 1. Set request’s response tainting to "basic".
            request->set_response_tainting(Infrastructure::Request::ResponseTainting::Basic);

            // 2. Return the result of running scheme fetch given fetchParams.
            return scheme_fetch(realm, fetch_params);

            // NOTE: HTML assigns any documents and workers created from URLs whose scheme is "data" a unique
            //       opaque origin. Service workers can only be created from URLs whose scheme is an HTTP(S) scheme.
        }
        // -> request’s mode is "same-origin"
        else if (request->mode() == Infrastructure::Request::Mode::SameOrigin) {
            // Return a network error.
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'same-origin' mode must have same URL and request origin"sv));
        }
        // -> request’s mode is "no-cors"
        else if (request->mode() == Infrastructure::Request::Mode::NoCORS) {
            // 1. If request’s redirect mode is not "follow", then return a network error.
            if (request->redirect_mode() != Infrastructure::Request::RedirectMode::Follow)
                return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'no-cors' mode must have redirect mode set to 'follow'"sv));

            // 2. Set request’s response tainting to "opaque".
            request->set_response_tainting(Infrastructure::Request::ResponseTainting::Opaque);

            // 3. Return the result of running scheme fetch given fetchParams.
            return scheme_fetch(realm, fetch_params);
        }
        // -> request’s current URL’s scheme is not an HTTP(S) scheme
        else if (!Infrastructure::is_http_or_https_scheme(request->current_url().scheme())) {
            // NOTE: At this point all other request modes have been handled. Ensure we're not lying in the error message :^)
            VERIFY(request->mode() == Infrastructure::Request::Mode::CORS);

            // Return a network error.
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'cors' mode must have URL with HTTP or HTTPS scheme"sv));
        }
        // -> request’s use-CORS-preflight flag is set
        // -> request’s unsafe-request flag is set and either request’s method is not a CORS-safelisted method or
        //    CORS-unsafe request-header names with request’s header list is not empty
        else if (
            request->use_cors_preflight()
            || (request->unsafe_request()
                && (!Infrastructure::is_cors_safelisted_method(request->method())
                    || !Infrastructure::get_cors_unsafe_header_names(request->header_list()).is_empty()))) {
            // 1. Set request’s response tainting to "cors".
            request->set_response_tainting(Infrastructure::Request::ResponseTainting::CORS);

            auto returned_pending_response = PendingResponse::create(vm, request);

            // 2. Let corsWithPreflightResponse be the result of running HTTP fetch given fetchParams and true.
            auto cors_with_preflight_response = TRY(http_fetch(realm, fetch_params, MakeCORSPreflight::Yes));
            cors_with_preflight_response->when_loaded([returned_pending_response](JS::NonnullGCPtr<Infrastructure::Response> cors_with_preflight_response) {
                dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'main fetch' cors_with_preflight_response load callback");
                // 3. If corsWithPreflightResponse is a network error, then clear cache entries using request.
                if (cors_with_preflight_response->is_network_error()) {
                    // FIXME: Clear cache entries
                }

                // 4. Return corsWithPreflightResponse.
                returned_pending_response->resolve(cors_with_preflight_response);
            });

            return returned_pending_response;
        }
        // -> Otherwise
        else {
            // 1. Set request’s response tainting to "cors".
            request->set_response_tainting(Infrastructure::Request::ResponseTainting::CORS);

            // 2. Return the result of running HTTP fetch given fetchParams.
            return http_fetch(realm, fetch_params);
        }
    };

    if (recursive == Recursive::Yes) {
        // 12. If response is null, then set response to the result of running the steps corresponding to the first
        //     matching statement:
        auto pending_response = !response
            ? TRY(get_response())
            : PendingResponse::create(vm, request, *response);

        // 13. If recursive is true, then return response.
        return pending_response;
    }

    // 11. If recursive is false, then run the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke([&realm, &vm, &fetch_params, request, response, get_response = move(get_response)] {
        // 12. If response is null, then set response to the result of running the steps corresponding to the first
        //     matching statement:
        auto pending_response = PendingResponse::create(vm, request, Infrastructure::Response::create(vm));
        if (!response) {
            auto pending_response_or_error = get_response();
            if (pending_response_or_error.is_error())
                return;
            pending_response = pending_response_or_error.release_value();
        }
        pending_response->when_loaded([&realm, &vm, &fetch_params, request, response, response_was_null = !response](JS::NonnullGCPtr<Infrastructure::Response> resolved_response) mutable {
            dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'main fetch' pending_response load callback");
            if (response_was_null)
                response = resolved_response;
            // 14. If response is not a network error and response is not a filtered response, then:
            if (!response->is_network_error() && !is<Infrastructure::FilteredResponse>(*response)) {
                // 1. If request’s response tainting is "cors", then:
                if (request->response_tainting() == Infrastructure::Request::ResponseTainting::CORS) {
                    // 1. Let headerNames be the result of extracting header list values given
                    //    `Access-Control-Expose-Headers` and response’s header list.
                    auto header_names_or_failure = Infrastructure::extract_header_list_values("Access-Control-Expose-Headers"sv.bytes(), response->header_list());
                    auto header_names = header_names_or_failure.has<Vector<ByteBuffer>>() ? header_names_or_failure.get<Vector<ByteBuffer>>() : Vector<ByteBuffer> {};

                    // 2. If request’s credentials mode is not "include" and headerNames contains `*`, then set
                    //    response’s CORS-exposed header-name list to all unique header names in response’s header
                    //    list.
                    if (request->credentials_mode() != Infrastructure::Request::CredentialsMode::Include && header_names.contains_slow("*"sv.bytes())) {
                        auto unique_header_names = response->header_list()->unique_names();
                        response->set_cors_exposed_header_name_list(move(unique_header_names));
                    }
                    // 3. Otherwise, if headerNames is not null or failure, then set response’s CORS-exposed
                    //    header-name list to headerNames.
                    else if (!header_names.is_empty()) {
                        response->set_cors_exposed_header_name_list(move(header_names));
                    }
                }

                // 2. Set response to the following filtered response with response as its internal response, depending
                //    on request’s response tainting:
                response = [&]() -> JS::NonnullGCPtr<Infrastructure::Response> {
                    switch (request->response_tainting()) {
                    // -> "basic"
                    case Infrastructure::Request::ResponseTainting::Basic:
                        // basic filtered response
                        return Infrastructure::BasicFilteredResponse::create(vm, *response);
                    // -> "cors"
                    case Infrastructure::Request::ResponseTainting::CORS:
                        // CORS filtered response
                        return Infrastructure::CORSFilteredResponse::create(vm, *response);
                    // -> "opaque"
                    case Infrastructure::Request::ResponseTainting::Opaque:
                        // opaque filtered response
                        return Infrastructure::OpaqueFilteredResponse::create(vm, *response);
                    default:
                        VERIFY_NOT_REACHED();
                    }
                }();
            }

            // 15. Let internalResponse be response, if response is a network error, and response’s internal response
            //     otherwise.
            auto internal_response = response->is_network_error()
                ? JS::NonnullGCPtr { *response }
                : static_cast<Infrastructure::FilteredResponse&>(*response).internal_response();

            // 16. If internalResponse’s URL list is empty, then set it to a clone of request’s URL list.
            // NOTE: A response’s URL list can be empty (for example, when the response represents an about URL).
            if (internal_response->url_list().is_empty())
                internal_response->set_url_list(request->url_list());

            // 17. If request has a redirect-tainted origin, then set internalResponse’s has-cross-origin-redirects to true.
            if (request->has_redirect_tainted_origin())
                internal_response->set_has_cross_origin_redirects(true);

            // 18. If request’s timing allow failed flag is unset, then set internalResponse’s timing allow passed flag.
            if (!request->timing_allow_failed())
                internal_response->set_timing_allow_passed(true);

            // 19. If response is not a network error and any of the following returns blocked
            if (!response->is_network_error() && (
                    // - should internalResponse to request be blocked as mixed content
                    MixedContent::should_response_to_request_be_blocked_as_mixed_content(request, internal_response) == Infrastructure::RequestOrResponseBlocking::Blocked
                    // FIXME: - should internalResponse to request be blocked by Content Security Policy
                    || false
                    // - should internalResponse to request be blocked due to its MIME type
                    || Infrastructure::should_response_to_request_be_blocked_due_to_its_mime_type(internal_response, request) == Infrastructure::RequestOrResponseBlocking::Blocked
                    // - should internalResponse to request be blocked due to nosniff
                    || Infrastructure::should_response_to_request_be_blocked_due_to_nosniff(internal_response, request) == Infrastructure::RequestOrResponseBlocking::Blocked)) {
                // then set response and internalResponse to a network error.
                response = internal_response = Infrastructure::Response::network_error(vm, "Response was blocked"_string);
            }

            // 20. If response’s type is "opaque", internalResponse’s status is 206, internalResponse’s range-requested
            //     flag is set, and request’s header list does not contain `Range`, then set response and
            //     internalResponse to a network error.
            // NOTE: Traditionally, APIs accept a ranged response even if a range was not requested. This prevents a
            //       partial response from an earlier ranged request being provided to an API that did not make a range
            //       request.
            if (response->type() == Infrastructure::Response::Type::Opaque
                && internal_response->status() == 206
                && internal_response->range_requested()
                && !request->header_list()->contains("Range"sv.bytes())) {
                response = internal_response = Infrastructure::Response::network_error(vm, "Response has status 206 and 'range-requested' flag set, but request has no 'Range' header"_string);
            }

            // 21. If response is not a network error and either request’s method is `HEAD` or `CONNECT`, or
            //     internalResponse’s status is a null body status, set internalResponse’s body to null and disregard
            //     any enqueuing toward it (if any).
            // NOTE: This standardizes the error handling for servers that violate HTTP.
            if (!response->is_network_error() && (StringView { request->method() }.is_one_of("HEAD"sv, "CONNECT"sv) || Infrastructure::is_null_body_status(internal_response->status())))
                internal_response->set_body({});

            // 22. If request’s integrity metadata is not the empty string, then:
            if (!request->integrity_metadata().is_empty()) {
                // 1. Let processBodyError be this step: run fetch response handover given fetchParams and a network
                //    error.
                auto process_body_error = JS::create_heap_function(vm.heap(), [&realm, &vm, &fetch_params](JS::Value) {
                    fetch_response_handover(realm, fetch_params, Infrastructure::Response::network_error(vm, "Response body could not be processed"sv));
                });

                // 2. If response’s body is null, then run processBodyError and abort these steps.
                if (!response->body()) {
                    process_body_error->function()({});
                    return;
                }

                // 3. Let processBody given bytes be these steps:
                auto process_body = JS::create_heap_function(vm.heap(), [&realm, request, response, &fetch_params, process_body_error = move(process_body_error)](ByteBuffer bytes) {
                    // 1. If bytes do not match request’s integrity metadata, then run processBodyError and abort these steps.
                    if (!TRY_OR_IGNORE(SRI::do_bytes_match_metadata_list(bytes, request->integrity_metadata()))) {
                        process_body_error->function()({});
                        return;
                    }

                    // 2. Set response’s body to bytes as a body.
                    response->set_body(TRY_OR_IGNORE(Infrastructure::byte_sequence_as_body(realm, bytes)));

                    // 3. Run fetch response handover given fetchParams and response.
                    fetch_response_handover(realm, fetch_params, *response);
                });

                // 4. Fully read response’s body given processBody and processBodyError.
                response->body()->fully_read(realm, process_body, process_body_error, fetch_params.task_destination());
            }
            // 23. Otherwise, run fetch response handover given fetchParams and response.
            else {
                fetch_response_handover(realm, fetch_params, *response);
            }
        });
    });

    return JS::GCPtr<PendingResponse> {};
}

// https://fetch.spec.whatwg.org/#fetch-finale
void fetch_response_handover(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params, Infrastructure::Response& response)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'fetch response handover' with: fetch_params @ {}, response @ {}", &fetch_params, &response);

    auto& vm = realm.vm();

    // 1. Let timingInfo be fetchParams’s timing info.
    auto timing_info = fetch_params.timing_info();

    // 2. If response is not a network error and fetchParams’s request’s client is a secure context, then set
    //    timingInfo’s server-timing headers to the result of getting, decoding, and splitting `Server-Timing` from
    //    response’s header list.
    //    The user agent may decide to expose `Server-Timing` headers to non-secure contexts requests as well.
    auto client = fetch_params.request()->client();
    if (!response.is_network_error() && client != nullptr && HTML::is_secure_context(*client)) {
        auto server_timing_headers = response.header_list()->get_decode_and_split("Server-Timing"sv.bytes());
        if (server_timing_headers.has_value())
            timing_info->set_server_timing_headers(server_timing_headers.release_value());
    }

    // 3. Let processResponseEndOfBody be the following steps:
    auto process_response_end_of_body = [&vm, &response, &fetch_params, timing_info] {
        // 1. Let unsafeEndTime be the unsafe shared current time.
        auto unsafe_end_time = HighResolutionTime::unsafe_shared_current_time();

        // 2. If fetchParams’s request’s destination is "document", then set fetchParams’s controller’s full timing
        //    info to fetchParams’s timing info.
        if (fetch_params.request()->destination() == Infrastructure::Request::Destination::Document)
            fetch_params.controller()->set_full_timing_info(fetch_params.timing_info());

        // 3. Set fetchParams’s controller’s report timing steps to the following steps given a global object global:
        fetch_params.controller()->set_report_timing_steps([&vm, &response, &fetch_params, timing_info, unsafe_end_time](JS::Object const& global) mutable {
            // 1. If fetchParams’s request’s URL’s scheme is not an HTTP(S) scheme, then return.
            if (!Infrastructure::is_http_or_https_scheme(fetch_params.request()->url().scheme()))
                return;

            // 2. Set timingInfo’s end time to the relative high resolution time given unsafeEndTime and global.
            timing_info->set_end_time(HighResolutionTime::relative_high_resolution_time(unsafe_end_time, global));

            // 3. Let cacheState be response’s cache state.
            auto cache_state = response.cache_state();

            // 4. Let bodyInfo be response’s body info.
            auto body_info = response.body_info();

            // 5. If response’s timing allow passed flag is not set, then set timingInfo to the result of creating an
            //    opaque timing info for timingInfo, set bodyInfo to a new response body info, and set cacheState to
            //    the empty string.
            // NOTE: This covers the case of response being a network error.
            if (!response.timing_allow_passed()) {
                timing_info = Infrastructure::create_opaque_timing_info(vm, timing_info);
                body_info = Infrastructure::Response::BodyInfo {};
                cache_state = {};
            }

            // 6. Let responseStatus be 0.
            auto response_status = 0;

            // 7. If fetchParams’s request’s mode is not "navigate" or response’s has-cross-origin-redirects is false:
            if (fetch_params.request()->mode() != Infrastructure::Request::Mode::Navigate || !response.has_cross_origin_redirects()) {
                // 1. Set responseStatus to response’s status.
                response_status = response.status();

                // 2. Let mimeType be the result of extracting a MIME type from response’s header list.
                auto mime_type = response.header_list()->extract_mime_type();

                // 3. If mimeType is non-null, then set bodyInfo’s content type to the result of minimizing a supported MIME type given mimeType.
                if (mime_type.has_value())
                    body_info.content_type = MimeSniff::minimise_a_supported_mime_type(mime_type.value());
            }

            // FIXME: 8. If fetchParams’s request’s initiator type is not null, then mark resource timing given timingInfo,
            //           request’s URL, request’s initiator type, global, cacheState, bodyInfo, and responseStatus.
            (void)timing_info;
            (void)global;
            (void)cache_state;
            (void)body_info;
            (void)response_status;
        });

        // 4. Let processResponseEndOfBodyTask be the following steps:
        auto process_response_end_of_body_task = JS::create_heap_function(vm.heap(), [&fetch_params, &response] {
            // 1. Set fetchParams’s request’s done flag.
            fetch_params.request()->set_done(true);

            // 2. If fetchParams’s process response end-of-body is non-null, then run fetchParams’s process response
            //    end-of-body given response.
            if (fetch_params.algorithms()->process_response_end_of_body())
                (fetch_params.algorithms()->process_response_end_of_body())(response);

            // 3. If fetchParams’s request’s initiator type is non-null and fetchParams’s request’s client’s global
            //    object is fetchParams’s task destination, then run fetchParams’s controller’s report timing steps
            //    given fetchParams’s request’s client’s global object.
            auto client = fetch_params.request()->client();
            auto const* task_destination_global_object = fetch_params.task_destination().get_pointer<JS::NonnullGCPtr<JS::Object>>();
            if (client != nullptr && task_destination_global_object != nullptr) {
                if (fetch_params.request()->initiator_type().has_value() && &client->global_object() == task_destination_global_object->ptr())
                    fetch_params.controller()->report_timing(client->global_object());
            }
        });

        // FIXME: Handle 'parallel queue' task destination
        auto task_destination = fetch_params.task_destination().get<JS::NonnullGCPtr<JS::Object>>();

        // 5. Queue a fetch task to run processResponseEndOfBodyTask with fetchParams’s task destination.
        Infrastructure::queue_fetch_task(fetch_params.controller(), task_destination, move(process_response_end_of_body_task));
    };

    // FIXME: Handle 'parallel queue' task destination
    auto task_destination = fetch_params.task_destination().get<JS::NonnullGCPtr<JS::Object>>();

    // 4. If fetchParams’s process response is non-null, then queue a fetch task to run fetchParams’s process response
    //    given response, with fetchParams’s task destination.
    if (fetch_params.algorithms()->process_response()) {
        Infrastructure::queue_fetch_task(fetch_params.controller(), task_destination, JS::create_heap_function(vm.heap(), [&fetch_params, &response]() {
            fetch_params.algorithms()->process_response()(response);
        }));
    }

    // 5. Let internalResponse be response, if response is a network error; otherwise response’s internal response.
    auto internal_response = response.is_network_error() ? JS::NonnullGCPtr { response } : response.unsafe_response();

    // 6. If internalResponse’s body is null, then run processResponseEndOfBody.
    if (!internal_response->body()) {
        process_response_end_of_body();
    }
    // 7. Otherwise:
    else {
        HTML::TemporaryExecutionContext const execution_context { Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let transformStream be a new TransformStream.
        auto transform_stream = realm.heap().allocate<Streams::TransformStream>(realm, realm);

        // 2. Let identityTransformAlgorithm be an algorithm which, given chunk, enqueues chunk in transformStream.
        auto identity_transform_algorithm = JS::create_heap_function(realm.heap(), [&realm, transform_stream](JS::Value chunk) -> JS::NonnullGCPtr<WebIDL::Promise> {
            MUST(Streams::transform_stream_default_controller_enqueue(*transform_stream->controller(), chunk));
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());
        });

        // 3. Set up transformStream with transformAlgorithm set to identityTransformAlgorithm and flushAlgorithm set
        //    to processResponseEndOfBody.
        auto flush_algorithm = JS::create_heap_function(realm.heap(), [&realm, process_response_end_of_body]() -> JS::NonnullGCPtr<WebIDL::Promise> {
            process_response_end_of_body();
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());
        });
        Streams::transform_stream_set_up(transform_stream, identity_transform_algorithm, flush_algorithm);

        // 4. Set internalResponse’s body’s stream to the result of internalResponse’s body’s stream piped through transformStream.
        auto promise = Streams::readable_stream_pipe_to(internal_response->body()->stream(), transform_stream->writable(), false, false, false, {});
        WebIDL::mark_promise_as_handled(*promise);
        internal_response->body()->set_stream(transform_stream->readable());
    }

    // 8. If fetchParams’s process response consume body is non-null, then:
    if (fetch_params.algorithms()->process_response_consume_body()) {
        // 1. Let processBody given nullOrBytes be this step: run fetchParams’s process response consume body given
        //    response and nullOrBytes.
        auto process_body = JS::create_heap_function(vm.heap(), [&fetch_params, &response](ByteBuffer null_or_bytes) {
            (fetch_params.algorithms()->process_response_consume_body())(response, null_or_bytes);
        });

        // 2. Let processBodyError be this step: run fetchParams’s process response consume body given response and
        //    failure.
        auto process_body_error = JS::create_heap_function(vm.heap(), [&fetch_params, &response](JS::Value) {
            (fetch_params.algorithms()->process_response_consume_body())(response, Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag {});
        });

        // 3. If internalResponse's body is null, then queue a fetch task to run processBody given null, with
        //    fetchParams’s task destination.
        if (!internal_response->body()) {
            Infrastructure::queue_fetch_task(fetch_params.controller(), task_destination, JS::create_heap_function(vm.heap(), [process_body = move(process_body)]() {
                process_body->function()({});
            }));
        }
        // 4. Otherwise, fully read internalResponse body given processBody, processBodyError, and fetchParams’s task
        //    destination.
        else {
            internal_response->body()->fully_read(realm, process_body, process_body_error, fetch_params.task_destination());
        }
    }
}

// https://fetch.spec.whatwg.org/#concept-scheme-fetch
WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> scheme_fetch(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'scheme fetch' with: fetch_params @ {}", &fetch_params);

    auto& vm = realm.vm();

    // 1. If fetchParams is canceled, then return the appropriate network error for fetchParams.
    if (fetch_params.is_canceled())
        return PendingResponse::create(vm, fetch_params.request(), Infrastructure::Response::appropriate_network_error(vm, fetch_params));

    // 2. Let request be fetchParams’s request.
    auto request = fetch_params.request();

    // 3. Switch on request’s current URL’s scheme and run the associated steps:
    // -> "about"
    if (request->current_url().scheme() == "about"sv) {
        // If request’s current URL’s path is the string "blank", then return a new response whose status message is
        // `OK`, header list is « (`Content-Type`, `text/html;charset=utf-8`) », and body is the empty byte sequence as
        // a body.
        // NOTE: URLs such as "about:config" are handled during navigation and result in a network error in the context
        //       of fetching.
        if (request->current_url().paths().size() == 1 && request->current_url().paths()[0] == "blank"sv) {
            auto response = Infrastructure::Response::create(vm);
            response->set_status_message(MUST(ByteBuffer::copy("OK"sv.bytes())));

            auto header = Infrastructure::Header::from_string_pair("Content-Type"sv, "text/html;charset=utf-8"sv);
            response->header_list()->append(move(header));

            response->set_body(MUST(Infrastructure::byte_sequence_as_body(realm, ""sv.bytes())));
            return PendingResponse::create(vm, request, response);
        }

        // FIXME: This is actually wrong, see note above.
        return TRY(nonstandard_resource_loader_file_or_http_network_fetch(realm, fetch_params));
    }
    // -> "blob"
    else if (request->current_url().scheme() == "blob"sv) {
        // 1. Let blobURLEntry be request’s current URL’s blob URL entry.
        auto const& blob_url_entry = request->current_url().blob_url_entry();

        // 2. If request’s method is not `GET`, blobURLEntry is null, or blobURLEntry’s object is not a Blob object,
        //    then return a network error. [FILEAPI]
        if (request->method() != "GET"sv.bytes() || !blob_url_entry.has_value()) {
            // FIXME: Handle "blobURLEntry’s object is not a Blob object". It could be a MediaSource object, but we
            //        have not yet implemented the Media Source Extensions spec.
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request has an invalid 'blob:' URL"sv));
        }

        // 3. Let blob be blobURLEntry’s object.
        auto const blob = FileAPI::Blob::create(realm, blob_url_entry.value().byte_buffer, blob_url_entry.value().type);

        // 4. Let response be a new response.
        auto response = Infrastructure::Response::create(vm);

        // 5. Let fullLength be blob’s size.
        auto full_length = blob->size();

        // 6. Let serializedFullLength be fullLength, serialized and isomorphic encoded.
        auto serialized_full_length = String::number(full_length);

        // 7. Let type be blob’s type.
        auto const& type = blob->type();

        // 8. If request’s header list does not contain `Range`:
        if (!request->header_list()->contains("Range"sv.bytes())) {
            // 1. Let bodyWithType be the result of safely extracting blob.
            auto body_with_type = TRY(safely_extract_body(realm, blob->raw_bytes()));

            // 2. Set response’s status message to `OK`.
            response->set_status_message(MUST(ByteBuffer::copy("OK"sv.bytes())));

            // 3. Set response’s body to bodyWithType’s body.
            response->set_body(move(body_with_type.body));

            // 4. Set response’s header list to « (`Content-Length`, serializedFullLength), (`Content-Type`, type) ».
            auto content_length_header = Infrastructure::Header::from_string_pair("Content-Length"sv, serialized_full_length);
            response->header_list()->append(move(content_length_header));

            auto content_type_header = Infrastructure::Header::from_string_pair("Content-Type"sv, type);
            response->header_list()->append(move(content_type_header));
        }
        // FIXME: 9. Otherwise:
        else {
            // 1. Set response’s range-requested flag.
            // 2. Let rangeHeader be the result of getting `Range` from request’s header list.
            // 3. Let rangeValue be the result of parsing a single range header value given rangeHeader and true.
            // 4. If rangeValue is failure, then return a network error.
            // 5. Let (rangeStart, rangeEnd) be rangeValue.
            // 6. If rangeStart is null:
            //     1. Set rangeStart to fullLength − rangeEnd.
            //     2. Set rangeEnd to rangeStart + rangeEnd − 1.
            // 7. Otherwise:
            //     1. If rangeStart is greater than or equal to fullLength, then return a network error.
            //     2. If rangeEnd is null or rangeEnd is greater than or equal to fullLength, then set rangeEnd to fullLength − 1.
            // 8. Let slicedBlob be the result of invoking slice blob given blob, rangeStart, rangeEnd + 1, and type.
            // 9. Let slicedBodyWithType be the result of safely extracting slicedBlob.
            // 10. Set response’s body to slicedBodyWithType’s body.
            // 11. Let serializedSlicedLength be slicedBlob’s size, serialized and isomorphic encoded.
            // 12. Let contentRange be `bytes `.
            // 13. Append rangeStart, serialized and isomorphic encoded, to contentRange.
            // 14. Append 0x2D (-) to contentRange.
            // 15. Append rangeEnd, serialized and isomorphic encoded to contentRange.
            // 16. Append 0x2F (/) to contentRange.
            // 17. Append serializedFullLength to contentRange.
            // 18. Set response’s status to 206.
            // 19. Set response’s status message to `Partial Content`.
            // 20. Set response’s header list to « (`Content-Length`, serializedSlicedLength), (`Content-Type`, type), (`Content-Range`, contentRange) ».
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request has a 'blob:' URL with a Content-Range header, which is currently unsupported"sv));
        }

        // 10. Return response.
        return PendingResponse::create(vm, request, response);
    }
    // -> "data"
    else if (request->current_url().scheme() == "data"sv) {
        // 1. Let dataURLStruct be the result of running the data: URL processor on request’s current URL.
        auto data_url_struct = Infrastructure::process_data_url(request->current_url());

        // 2. If dataURLStruct is failure, then return a network error.
        if (data_url_struct.is_error())
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Failed to process 'data:' URL"sv));

        // 3. Let mimeType be dataURLStruct’s MIME type, serialized.
        auto const& mime_type = data_url_struct.value().mime_type.serialized();

        // 4. Return a new response whose status message is `OK`, header list is « (`Content-Type`, mimeType) », and
        //    body is dataURLStruct’s body as a body.
        auto response = Infrastructure::Response::create(vm);
        response->set_status_message(MUST(ByteBuffer::copy("OK"sv.bytes())));

        auto header = Infrastructure::Header::from_string_pair("Content-Type"sv, mime_type);
        response->header_list()->append(move(header));

        response->set_body(TRY(Infrastructure::byte_sequence_as_body(realm, data_url_struct.value().body)));
        return PendingResponse::create(vm, request, response);
    }
    // -> "file"
    // AD-HOC: "resource"
    else if (request->current_url().scheme() == "file"sv || request->current_url().scheme() == "resource"sv) {
        // For now, unfortunate as it is, file: URLs are left as an exercise for the reader.
        // When in doubt, return a network error.
        if (request->origin().has<URL::Origin>() && (request->origin().get<URL::Origin>().is_opaque() || request->origin().get<URL::Origin>().scheme() == "file"sv || request->origin().get<URL::Origin>().scheme() == "resource"sv))
            return TRY(nonstandard_resource_loader_file_or_http_network_fetch(realm, fetch_params));
        else
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'file:' or 'resource:' URL blocked"sv));
    }
    // -> HTTP(S) scheme
    else if (Infrastructure::is_http_or_https_scheme(request->current_url().scheme())) {
        // Return the result of running HTTP fetch given fetchParams.
        return http_fetch(realm, fetch_params);
    }

    // 4. Return a network error.
    auto message = request->current_url().scheme() == "about"sv
        ? "Request has invalid 'about:' URL, only 'about:blank' can be fetched"_string
        : "Request URL has invalid scheme, must be one of 'about', 'blob', 'data', 'file', 'http', or 'https'"_string;
    return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, move(message)));
}

// https://fetch.spec.whatwg.org/#concept-http-fetch
WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> http_fetch(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params, MakeCORSPreflight make_cors_preflight)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP fetch' with: fetch_params @ {}, make_cors_preflight = {}",
        &fetch_params, make_cors_preflight == MakeCORSPreflight::Yes ? "Yes"sv : "No"sv);

    auto& vm = realm.vm();

    // 1. Let request be fetchParams’s request.
    auto request = fetch_params.request();

    // 2. Let response and internalResponse be null.
    JS::GCPtr<Infrastructure::Response> response;
    JS::GCPtr<Infrastructure::Response> internal_response;

    // 3. If request’s service-workers mode is "all", then:
    if (request->service_workers_mode() == Infrastructure::Request::ServiceWorkersMode::All) {
        // 1. Let requestForServiceWorker be a clone of request.
        auto request_for_service_worker = request->clone(realm);

        // 2. If requestForServiceWorker’s body is non-null, then:
        if (!request_for_service_worker->body().has<Empty>()) {
            // FIXME: 1. Let transformStream be a new TransformStream.
            // FIXME: 2. Let transformAlgorithm given chunk be these steps:
            // FIXME: 3. Set up transformStream with transformAlgorithm set to transformAlgorithm.
            // FIXME: 4. Set requestForServiceWorker’s body’s stream to the result of requestForServiceWorker’s body’s stream
            //           piped through transformStream.
        }

        // 3. Let serviceWorkerStartTime be the coarsened shared current time given fetchParams’s cross-origin isolated
        //    capability.
        auto service_worker_start_time = HighResolutionTime::coarsened_shared_current_time(fetch_params.cross_origin_isolated_capability() == HTML::CanUseCrossOriginIsolatedAPIs::Yes);

        // FIXME: 4. Set response to the result of invoking handle fetch for requestForServiceWorker, with fetchParams’s
        //           controller and fetchParams’s cross-origin isolated capability.

        // 5. If response is non-null, then:
        if (response) {
            // 1. Set fetchParams’s timing info’s final service worker start time to serviceWorkerStartTime.
            fetch_params.timing_info()->set_final_service_worker_start_time(service_worker_start_time);

            // 2. If request’s body is non-null, then cancel request’s body with undefined.
            if (!request->body().has<Empty>()) {
                // FIXME: Implement cancelling streams
            }

            // 3. Set internalResponse to response, if response is not a filtered response; otherwise to response’s
            //    internal response.
            internal_response = !is<Infrastructure::FilteredResponse>(*response)
                ? JS::NonnullGCPtr { *response }
                : static_cast<Infrastructure::FilteredResponse const&>(*response).internal_response();

            // 4. If one of the following is true
            if (
                // - response’s type is "error"
                response->type() == Infrastructure::Response::Type::Error
                // - request’s mode is "same-origin" and response’s type is "cors"
                || (request->mode() == Infrastructure::Request::Mode::SameOrigin && response->type() == Infrastructure::Response::Type::CORS)
                // - request’s mode is not "no-cors" and response’s type is "opaque"
                || (request->mode() != Infrastructure::Request::Mode::NoCORS && response->type() == Infrastructure::Response::Type::Opaque)
                // - request’s redirect mode is not "manual" and response’s type is "opaqueredirect"
                || (request->redirect_mode() != Infrastructure::Request::RedirectMode::Manual && response->type() == Infrastructure::Response::Type::OpaqueRedirect)
                // - request’s redirect mode is not "follow" and response’s URL list has more than one item.
                || (request->redirect_mode() != Infrastructure::Request::RedirectMode::Follow && response->url_list().size() > 1)) {
                // then return a network error.
                return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Invalid request/response state combination"sv));
            }
        }
    }

    JS::GCPtr<PendingResponse> pending_actual_response;

    auto returned_pending_response = PendingResponse::create(vm, request);

    // 4. If response is null, then:
    if (!response) {
        // 1. If makeCORSPreflight is true and one of these conditions is true:
        // NOTE: This step checks the CORS-preflight cache and if there is no suitable entry it performs a
        //       CORS-preflight fetch which, if successful, populates the cache. The purpose of the CORS-preflight
        //       fetch is to ensure the fetched resource is familiar with the CORS protocol. The cache is there to
        //       minimize the number of CORS-preflight fetches.
        JS::GCPtr<PendingResponse> pending_preflight_response;
        if (make_cors_preflight == MakeCORSPreflight::Yes && (
                // - There is no method cache entry match for request’s method using request, and either request’s
                //   method is not a CORS-safelisted method or request’s use-CORS-preflight flag is set.
                //   FIXME: We currently have no cache, so there will always be no method cache entry.
                (!Infrastructure::is_cors_safelisted_method(request->method()) || request->use_cors_preflight())
                // - There is at least one item in the CORS-unsafe request-header names with request’s header list for
                //   which there is no header-name cache entry match using request.
                //   FIXME: We currently have no cache, so there will always be no header-name cache entry.
                || !Infrastructure::get_cors_unsafe_header_names(request->header_list()).is_empty())) {
            // 1. Let preflightResponse be the result of running CORS-preflight fetch given request.
            pending_preflight_response = TRY(cors_preflight_fetch(realm, request));

            // NOTE: Step 2 is performed in pending_preflight_response's load callback below.
        }

        auto fetch_main_content = [request = JS::make_handle(request), realm = JS::make_handle(realm), fetch_params = JS::make_handle(fetch_params)]() -> WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> {
            // 2. If request’s redirect mode is "follow", then set request’s service-workers mode to "none".
            // NOTE: Redirects coming from the network (as opposed to from a service worker) are not to be exposed to a
            //       service worker.
            if (request->redirect_mode() == Infrastructure::Request::RedirectMode::Follow)
                request->set_service_workers_mode(Infrastructure::Request::ServiceWorkersMode::None);

            // 3. Set response and internalResponse to the result of running HTTP-network-or-cache fetch given fetchParams.
            return http_network_or_cache_fetch(*realm, *fetch_params);
        };

        if (pending_preflight_response) {
            pending_actual_response = PendingResponse::create(vm, request);
            pending_preflight_response->when_loaded([returned_pending_response, pending_actual_response, fetch_main_content = move(fetch_main_content)](JS::NonnullGCPtr<Infrastructure::Response> preflight_response) {
                dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP fetch' pending_preflight_response load callback");

                // 2. If preflightResponse is a network error, then return preflightResponse.
                if (preflight_response->is_network_error()) {
                    returned_pending_response->resolve(preflight_response);
                    return;
                }

                auto pending_main_content_response = TRY_OR_IGNORE(fetch_main_content());
                pending_main_content_response->when_loaded([pending_actual_response](JS::NonnullGCPtr<Infrastructure::Response> main_content_response) {
                    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP fetch' pending_main_content_response load callback");
                    pending_actual_response->resolve(main_content_response);
                });
            });
        } else {
            pending_actual_response = TRY(fetch_main_content());
        }
    } else {
        pending_actual_response = PendingResponse::create(vm, request, Infrastructure::Response::create(vm));
    }

    pending_actual_response->when_loaded([&realm, &vm, &fetch_params, request, response, internal_response, returned_pending_response, response_was_null = !response](JS::NonnullGCPtr<Infrastructure::Response> resolved_actual_response) mutable {
        dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP fetch' pending_actual_response load callback");
        if (response_was_null) {
            response = internal_response = resolved_actual_response;
            // 4. If request’s response tainting is "cors" and a CORS check for request and response returns failure,
            //    then return a network error.
            // NOTE: As the CORS check is not to be applied to responses whose status is 304 or 407, or responses from
            //       a service worker for that matter, it is applied here.
            if (request->response_tainting() == Infrastructure::Request::ResponseTainting::CORS
                && !cors_check(request, *response)) {
                returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "Request with 'cors' response tainting failed CORS check"_string));
                return;
            }

            // 5. If the TAO check for request and response returns failure, then set request’s timing allow failed flag.
            if (!tao_check(request, *response))
                request->set_timing_allow_failed(true);
        }

        // 5. If either request’s response tainting or response’s type is "opaque", and the cross-origin resource
        //    policy check with request’s origin, request’s client, request’s destination, and internalResponse returns
        //    blocked, then return a network error.
        // NOTE: The cross-origin resource policy check runs for responses coming from the network and responses coming
        //       from the service worker. This is different from the CORS check, as request’s client and the service
        //       worker can have different embedder policies.
        if ((request->response_tainting() == Infrastructure::Request::ResponseTainting::Opaque || response->type() == Infrastructure::Response::Type::Opaque)
            && false // FIXME: "and the cross-origin resource policy check with request’s origin, request’s client, request’s destination, and actualResponse returns blocked"
        ) {
            returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "Response was blocked by cross-origin resource policy check"_string));
            return;
        }

        JS::GCPtr<PendingResponse> inner_pending_response;

        // 6. If internalResponse’s status is a redirect status:
        if (Infrastructure::is_redirect_status(internal_response->status())) {
            // FIXME: 1. If internalResponse’s status is not 303, request’s body is non-null, and the connection uses HTTP/2,
            //           then user agents may, and are even encouraged to, transmit an RST_STREAM frame.
            // NOTE: 303 is excluded as certain communities ascribe special status to it.

            // 2. Switch on request’s redirect mode:
            switch (request->redirect_mode()) {
            // -> "error"
            case Infrastructure::Request::RedirectMode::Error:
                // 1. Set response to a network error.
                response = Infrastructure::Response::network_error(vm, "Request with 'error' redirect mode received redirect response"_string);
                break;
            // -> "manual"
            case Infrastructure::Request::RedirectMode::Manual:
                // 1. If request’s mode is "navigate", then set fetchParams’s controller’s next manual redirect steps
                //    to run HTTP-redirect fetch given fetchParams and response.
                if (request->mode() == Infrastructure::Request::Mode::Navigate) {
                    fetch_params.controller()->set_next_manual_redirect_steps([&realm, &fetch_params, response] {
                        (void)http_redirect_fetch(realm, fetch_params, *response);
                    });
                }
                // 2. Otherwise, set response to an opaque-redirect filtered response whose internal response is
                //    internalResponse.
                else {
                    response = Infrastructure::OpaqueRedirectFilteredResponse::create(vm, *internal_response);
                }
                break;
            // -> "follow"
            case Infrastructure::Request::RedirectMode::Follow:
                // 1. Set response to the result of running HTTP-redirect fetch given fetchParams and response.
                inner_pending_response = TRY_OR_IGNORE(http_redirect_fetch(realm, fetch_params, *response));
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        if (inner_pending_response) {
            inner_pending_response->when_loaded([returned_pending_response](JS::NonnullGCPtr<Infrastructure::Response> response) {
                dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP fetch' inner_pending_response load callback");
                returned_pending_response->resolve(response);
            });
        } else {
            returned_pending_response->resolve(*response);
        }
    });

    // 7. Return response.
    // NOTE: Typically internalResponse’s body’s stream is still being enqueued to after returning.
    return returned_pending_response;
}

// https://fetch.spec.whatwg.org/#concept-http-redirect-fetch
WebIDL::ExceptionOr<JS::GCPtr<PendingResponse>> http_redirect_fetch(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params, Infrastructure::Response& response)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP-redirect fetch' with: fetch_params @ {}, response = {}", &fetch_params, &response);

    auto& vm = realm.vm();

    // 1. Let request be fetchParams’s request.
    auto request = fetch_params.request();

    // 2. Let internalResponse be response, if response is not a filtered response; otherwise response’s internal
    //    response.
    auto internal_response = !is<Infrastructure::FilteredResponse>(response)
        ? JS::NonnullGCPtr { response }
        : static_cast<Infrastructure::FilteredResponse const&>(response).internal_response();

    // 3. Let locationURL be internalResponse’s location URL given request’s current URL’s fragment.
    auto location_url_or_error = internal_response->location_url(request->current_url().fragment());

    // 4. If locationURL is null, then return response.
    if (!location_url_or_error.is_error() && !location_url_or_error.value().has_value())
        return PendingResponse::create(vm, request, response);

    // 5. If locationURL is failure, then return a network error.
    if (location_url_or_error.is_error())
        return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request redirect URL is invalid"sv));

    auto location_url = location_url_or_error.release_value().release_value();

    // 6. If locationURL’s scheme is not an HTTP(S) scheme, then return a network error.
    if (!Infrastructure::is_http_or_https_scheme(location_url.scheme()))
        return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request redirect URL must have HTTP or HTTPS scheme"sv));

    // 7. If request’s redirect count is 20, then return a network error.
    if (request->redirect_count() == 20)
        return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request has reached maximum redirect count of 20"sv));

    // 8. Increase request’s redirect count by 1.
    request->set_redirect_count(request->redirect_count() + 1);

    // 9. If request’s mode is "cors", locationURL includes credentials, and request’s origin is not same origin with
    //    locationURL’s origin, then return a network error.
    if (request->mode() == Infrastructure::Request::Mode::CORS
        && location_url.includes_credentials()
        && request->origin().has<URL::Origin>()
        && !request->origin().get<URL::Origin>().is_same_origin(location_url.origin())) {
        return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'cors' mode and different URL and request origin must not include credentials in redirect URL"sv));
    }

    // 10. If request’s response tainting is "cors" and locationURL includes credentials, then return a network error.
    // NOTE: This catches a cross-origin resource redirecting to a same-origin URL.
    if (request->response_tainting() == Infrastructure::Request::ResponseTainting::CORS && location_url.includes_credentials())
        return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'cors' response tainting must not include credentials in redirect URL"sv));

    // 11. If internalResponse’s status is not 303, request’s body is non-null, and request’s body’s source is null, then
    //     return a network error.
    if (internal_response->status() != 303
        && !request->body().has<Empty>()
        && request->body().get<JS::NonnullGCPtr<Infrastructure::Body>>()->source().has<Empty>()) {
        return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request has body but no body source"sv));
    }

    // 12. If one of the following is true
    if (
        // - internalResponse’s status is 301 or 302 and request’s method is `POST`
        ((internal_response->status() == 301 || internal_response->status() == 302) && request->method() == "POST"sv.bytes())
        // - internalResponse’s status is 303 and request’s method is not `GET` or `HEAD`
        || (internal_response->status() == 303 && !(request->method() == "GET"sv.bytes() || request->method() == "HEAD"sv.bytes()))
        // then:
    ) {
        // 1. Set request’s method to `GET` and request’s body to null.
        request->set_method(MUST(ByteBuffer::copy("GET"sv.bytes())));
        request->set_body({});

        static constexpr Array request_body_header_names {
            "Content-Encoding"sv,
            "Content-Language"sv,
            "Content-Location"sv,
            "Content-Type"sv
        };
        // 2. For each headerName of request-body-header name, delete headerName from request’s header list.
        for (auto header_name : request_body_header_names.span())
            request->header_list()->delete_(header_name.bytes());
    }

    // 13. If request’s current URL’s origin is not same origin with locationURL’s origin, then for each headerName of
    //     CORS non-wildcard request-header name, delete headerName from request’s header list.
    // NOTE: I.e., the moment another origin is seen after the initial request, the `Authorization` header is removed.
    if (!request->current_url().origin().is_same_origin(location_url.origin())) {
        static constexpr Array cors_non_wildcard_request_header_names {
            "Authorization"sv
        };
        for (auto header_name : cors_non_wildcard_request_header_names)
            request->header_list()->delete_(header_name.bytes());
    }

    // 14. If request’s body is non-null, then set request’s body to the body of the result of safely extracting
    //     request’s body’s source.
    // NOTE: request’s body’s source’s nullity has already been checked.
    if (!request->body().has<Empty>()) {
        auto const& source = request->body().get<JS::NonnullGCPtr<Infrastructure::Body>>()->source();
        // NOTE: BodyInitOrReadableBytes is a superset of Body::SourceType
        auto converted_source = source.has<ByteBuffer>()
            ? BodyInitOrReadableBytes { source.get<ByteBuffer>() }
            : BodyInitOrReadableBytes { source.get<JS::Handle<FileAPI::Blob>>() };
        auto [body, _] = TRY(safely_extract_body(realm, converted_source));
        request->set_body(move(body));
    }

    // 15. Let timingInfo be fetchParams’s timing info.
    auto timing_info = fetch_params.timing_info();

    // 16. Set timingInfo’s redirect end time and post-redirect start time to the coarsened shared current time given
    //     fetchParams’s cross-origin isolated capability.
    auto now = HighResolutionTime::coarsened_shared_current_time(fetch_params.cross_origin_isolated_capability() == HTML::CanUseCrossOriginIsolatedAPIs::Yes);
    timing_info->set_redirect_end_time(now);
    timing_info->set_post_redirect_start_time(now);

    // 17. If timingInfo’s redirect start time is 0, then set timingInfo’s redirect start time to timingInfo’s start
    //     time.
    if (timing_info->redirect_start_time() == 0)
        timing_info->set_redirect_start_time(timing_info->start_time());

    // 18. Append locationURL to request’s URL list.
    request->url_list().append(location_url);

    // 19. Invoke set request’s referrer policy on redirect on request and internalResponse.
    ReferrerPolicy::set_request_referrer_policy_on_redirect(request, internal_response);

    // 20. Let recursive be true.
    auto recursive = Recursive::Yes;

    // 21. If request’s redirect mode is "manual", then:
    if (request->redirect_mode() == Infrastructure::Request::RedirectMode::Manual) {
        // 1. Assert: request’s mode is "navigate".
        VERIFY(request->mode() == Infrastructure::Request::Mode::Navigate);

        // 2. Set recursive to false.
        recursive = Recursive::No;
    }

    // 22. Return the result of running main fetch given fetchParams and recursive.
    return main_fetch(realm, fetch_params, recursive);
}

class CachePartition : public RefCounted<CachePartition> {
public:
    // https://httpwg.org/specs/rfc9111.html#constructing.responses.from.caches
    JS::GCPtr<Infrastructure::Response> select_response(URL::URL const& url, ReadonlyBytes method, Vector<Infrastructure::Header> const& headers, Vector<JS::GCPtr<Infrastructure::Response>>& initial_set_of_stored_responses) const
    {
        // When presented with a request, a cache MUST NOT reuse a stored response unless:

        // - the presented target URI (Section 7.1 of [HTTP]) and that of the stored response match, and
        auto it = m_cache.find(url);
        if (it == m_cache.end()) {
            dbgln("\033[31;1mHTTP CACHE MISS!\033[0m {}", url);
            return {};
        }
        auto const& cached_response = it->value;

        // - the request method associated with the stored response allows it to be used for the presented request, and
        if (method != cached_response->method()) {
            dbgln("\033[31;1mHTTP CACHE MISS!\033[0m (Bad method) {}", url);
            return {};
        }

        // FIXME: - request header fields nominated by the stored response (if any) match those presented (see Section 4.1), and
        (void)headers;

        // FIXME: - the stored response does not contain the no-cache directive (Section 5.2.2.4), unless it is successfully validated (Section 4.3), and

        initial_set_of_stored_responses.append(cached_response);

        // FIXME: - the stored response is one of the following:
        //          + fresh (see Section 4.2), or
        //          + allowed to be served stale (see Section 4.2.4), or
        //          + successfully validated (see Section 4.3).

        dbgln("\033[32;1mHTTP CACHE HIT!\033[0m {}", url);

        return cached_response;
    }

    void store_response(JS::Realm& realm, Infrastructure::Request const& http_request, Infrastructure::Response const& response)
    {
        if (!is_cacheable(http_request, response))
            return;

        auto cached_response = Infrastructure::Response::create(realm.vm());

        store_header_and_trailer_fields(response, *cached_response->header_list());
        cached_response->set_body(response.body()->clone(realm));
        cached_response->set_body_info(response.body_info());
        cached_response->set_method(MUST(ByteBuffer::copy(http_request.method())));
        cached_response->set_status(response.status());
        cached_response->url_list().append(http_request.current_url());
        m_cache.set(http_request.current_url(), move(cached_response));
    }

    // https://httpwg.org/specs/rfc9111.html#freshening.responses
    void freshen_stored_responses_upon_validation(Infrastructure::Response const& response, Vector<JS::GCPtr<Infrastructure::Response>>& initial_set_of_stored_responses)
    {
        // When a cache receives a 304 (Not Modified) response, it needs to identify stored
        // responses that are suitable for updating with the new information provided, and then do so.

        // The initial set of stored responses to update are those that could have been
        // chosen for that request — i.e., those that meet the requirements in Section 4,
        // except the last requirement to be fresh, able to be served stale, or just validated.
        for (auto stored_response : initial_set_of_stored_responses) {
            // Then, that initial set of stored responses is further filtered by the first match of:

            // - FIXME: If the new response contains one or more strong validators (see Section 8.8.1 of [HTTP]),
            //   then each of those strong validators identifies a selected representation for update.
            //   All the stored responses in the initial set with one of those same strong validators
            //   are identified for update.
            //   If none of the initial set contains at least one of the same strong validators,
            //   then the cache MUST NOT use the new response to update any stored responses.
            // - FIXME: If the new response contains no strong validators but does contain one or more weak validators,
            //   and those validators correspond to one of the initial set's stored responses,
            //   then the most recent of those matching stored responses is identified for update.
            // - FIXME: If the new response does not include any form of validator (such as where a client generates an
            //   `If-Modified-Since` request from a source other than the `Last-Modified` response header field),
            //   and there is only one stored response in the initial set, and that stored response also lacks a validator,
            //   then that stored response is identified for update.

            // For each stored response identified, the cache MUST update its header fields
            // with the header fields provided in the 304 (Not Modified) response, as per Section 3.2.
            update_stored_header_fields(response, stored_response->header_list());
        }
    }

private:
    // https://httpwg.org/specs/rfc9111.html#storing.fields
    bool is_exempted_for_storage(StringView header_name)
    {
        // Caches MUST include all received response header fields — including unrecognized ones — when storing a response;
        // this assures that new HTTP header fields can be successfully deployed. However, the following exceptions are made:

        // - The Connection header field and fields whose names are listed in it are required by Section 7.6.1 of [HTTP]
        //   to be removed before forwarding the message. This MAY be implemented by doing so before storage.

        // - Likewise, some fields' semantics require them to be removed before forwarding the message, and this MAY be
        //   implemented by doing so before storage; see Section 7.6.1 of [HTTP] for some examples.

        // FIXME: - The no-cache (Section 5.2.2.4) and private (Section 5.2.2.7) cache directives can have arguments that
        //          prevent storage of header fields by all caches and shared caches, respectively.

        // FIXME: - Header fields that are specific to the proxy that a cache uses when forwarding a request MUST NOT be stored,
        //          unless the cache incorporates the identity of the proxy into the cache key.
        //          Effectively, this is limited to Proxy-Authenticate (Section 11.7.1 of [HTTP]), Proxy-Authentication-Info (Section 11.7.3 of [HTTP]), and Proxy-Authorization (Section 11.7.2 of [HTTP]).

        return header_name.is_one_of_ignoring_ascii_case(
            "Connection"sv,
            "Proxy-Connection"sv,
            "Keep-Alive"sv,
            "TE"sv,
            "Transfer-Encoding"sv,
            "Upgrade"sv);
    }

    // https://httpwg.org/specs/rfc9111.html#update
    bool is_exempted_for_updating(StringView header_name)
    {
        // Caches are required to update a stored response's header fields from another
        // (typically newer) response in several situations; for example, see Sections 3.4, 4.3.4, and 4.3.5.

        // When doing so, the cache MUST add each header field in the provided response to the stored response,
        // replacing field values that are already present, with the following exceptions:

        // - Header fields excepted from storage in Section 3.1,
        return is_exempted_for_storage(header_name)
            // - Header fields that the cache's stored response depends upon, as described below,
            || false
            // - Header fields that are automatically processed and removed by the recipient, as described below, and
            || false
            // - The Content-Length header field.
            || header_name.equals_ignoring_ascii_case("Content-Length"sv);

        // In some cases, caches (especially in user agents) store the results of processing
        // the received response, rather than the response itself, and updating header fields
        // that affect that processing can result in inconsistent behavior and security issues.
        // Caches in this situation MAY omit these header fields from updating stored responses
        // on an exceptional basis but SHOULD limit such omission to those fields necessary to
        // assure integrity of the stored response.

        // For example, a browser might decode the content coding of a response while it is being received,
        // creating a disconnect between the data it has stored and the response's original metadata.
        // Updating that stored metadata with a different Content-Encoding header field would be problematic.
        // Likewise, a browser might store a post-parse HTML tree rather than the content received in the response;
        // updating the Content-Type header field would not be workable in this case because any assumptions about
        // the format made in parsing would now be invalid.

        // Furthermore, some fields are automatically processed and removed by the HTTP implementation,
        // such as the Content-Range header field. Implementations MAY automatically omit such header fields from updates,
        // even when the processing does not actually occur.

        // Note that the Content-* prefix is not a signal that a header field is omitted from update; it is a convention for MIME header fields, not HTTP.
    }

    // https://httpwg.org/specs/rfc9111.html#update
    void update_stored_header_fields(Infrastructure::Response const& response, Infrastructure::HeaderList& headers)
    {
        for (auto& header : *response.header_list()) {
            auto name = StringView(header.name);

            if (is_exempted_for_updating(name))
                continue;

            headers.delete_(header.name);
        }

        for (auto& header : *response.header_list()) {
            auto name = StringView(header.name);

            if (is_exempted_for_updating(name))
                continue;

            headers.append(Infrastructure::Header::copy(header));
        }
    }

    // https://httpwg.org/specs/rfc9111.html#storing.fields
    void store_header_and_trailer_fields(Infrastructure::Response const& response, Web::Fetch::Infrastructure::HeaderList& headers)
    {
        for (auto& header : *response.header_list()) {
            auto name = StringView(header.name);

            if (is_exempted_for_storage(name))
                continue;

            headers.append(Infrastructure::Header::copy(header));
        }
    }

    // https://httpwg.org/specs/rfc9111.html#response.cacheability
    static bool is_cacheable(Infrastructure::Request const& request, Infrastructure::Response const& response)
    {
        // A cache MUST NOT store a response to a request unless:

        // - AD-HOC: For now, we simply don't cache responses without a simple ByteBuffer body.
        if (!response.body() || !response.body()->source().has<ByteBuffer>())
            return false;

        // - the request method is understood by the cache;
        if (request.method() != "GET"sv.bytes() && request.method() != "HEAD"sv.bytes())
            return false;

        // - the response status code is final (see Section 15 of [HTTP]);
        if (response.status() < 200)
            return false;

        // - if the response status code is 206 or 304,
        //   or the must-understand cache directive (see Section 5.2.2.3) is present:
        //       the cache understands the response status code;
        if (response.status() == 206 || response.status() == 304) {
            // FIXME: Implement must-understand cache directive
        }

        // - the no-store cache directive is not present in the response (see Section 5.2.2.5);
        if (request.cache_mode() == Infrastructure::Request::CacheMode::NoStore)
            return false;

        // FIXME: - if the cache is shared: the private response directive is either not present
        //          or allows a shared cache to store a modified response; see Section 5.2.2.7);

        // FIXME: - if the cache is shared: the Authorization header field is not present in the
        //          request (see Section 11.6.2 of [HTTP]) or a response directive is present
        //          that explicitly allows shared caching (see Section 3.5); and

        // FIXME: - the response contains at least one of the following:
        //          + a public response directive (see Section 5.2.2.9);
        //          + a private response directive, if the cache is not shared (see Section 5.2.2.7);
        //          + an Expires header field (see Section 5.3);
        //          + a max-age response directive (see Section 5.2.2.1);
        //          + if the cache is shared: an s-maxage response directive (see Section 5.2.2.10);
        //          + a cache extension that allows it to be cached (see Section 5.2.3); or
        //          + a status code that is defined as heuristically cacheable (see Section 4.2.2).

        return true;
    }

    HashMap<URL::URL, JS::GCPtr<Infrastructure::Response>> m_cache;
};

class HTTPCache {
public:
    CachePartition& get(Infrastructure::NetworkPartitionKey const& key)
    {
        return *m_cache.ensure(key, [] {
            return adopt_ref(*new CachePartition);
        });
    }

    static HTTPCache& the()
    {
        static HTTPCache s_cache;
        return s_cache;
    }

private:
    HashMap<Infrastructure::NetworkPartitionKey, NonnullRefPtr<CachePartition>> m_cache;
};

// https://fetch.spec.whatwg.org/#determine-the-http-cache-partition
static RefPtr<CachePartition> determine_the_http_cache_partition(Infrastructure::Request const& request)
{
    if (!g_http_cache_enabled)
        return nullptr;

    // 1. Let key be the result of determining the network partition key given request.
    auto key = Infrastructure::determine_the_network_partition_key(request);

    // 2. If key is null, then return null.
    if (!key.has_value())
        return nullptr;

    // 3. Return the unique HTTP cache associated with key. [HTTP-CACHING]
    return HTTPCache::the().get(key.value());
}

// https://fetch.spec.whatwg.org/#concept-http-network-or-cache-fetch
WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> http_network_or_cache_fetch(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params, IsAuthenticationFetch is_authentication_fetch, IsNewConnectionFetch is_new_connection_fetch)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP-network-or-cache fetch' with: fetch_params @ {}, is_authentication_fetch = {}, is_new_connection_fetch = {}",
        &fetch_params, is_authentication_fetch == IsAuthenticationFetch::Yes ? "Yes"sv : "No"sv, is_new_connection_fetch == IsNewConnectionFetch::Yes ? "Yes"sv : "No"sv);

    auto& vm = realm.vm();

    // 1. Let request be fetchParams’s request.
    auto request = fetch_params.request();

    // 2. Let httpFetchParams be null.
    JS::GCPtr<Infrastructure::FetchParams const> http_fetch_params;

    // 3. Let httpRequest be null.
    JS::GCPtr<Infrastructure::Request> http_request;

    // 4. Let response be null.
    JS::GCPtr<Infrastructure::Response> response;

    // 5. Let storedResponse be null.
    JS::GCPtr<Infrastructure::Response> stored_response;
    Vector<JS::GCPtr<Infrastructure::Response>> initial_set_of_stored_responses;

    // 6. Let httpCache be null.
    // (Typeless until we actually implement it, needed for checks below)
    RefPtr<CachePartition> http_cache;

    // 7. Let the revalidatingFlag be unset.
    auto revalidating_flag = RefCountedFlag::create(false);

    auto include_credentials = IncludeCredentials::No;

    // 8. Run these steps, but abort when fetchParams is canceled:
    // NOTE: There's an 'if aborted' check after this anyway, so not doing this is fine and only incurs a small delay.
    //       For now, support for aborting fetch requests is limited anyway as ResourceLoader doesn't support it.
    auto aborted = false;
    {
        ScopeGuard set_aborted = [&] {
            if (fetch_params.is_canceled())
                aborted = true;
        };

        // 1. If request’s window is "no-window" and request’s redirect mode is "error", then set httpFetchParams to
        //    fetchParams and httpRequest to request.
        if (request->window().has<Infrastructure::Request::Window>()
            && request->window().get<Infrastructure::Request::Window>() == Infrastructure::Request::Window::NoWindow
            && request->redirect_mode() == Infrastructure::Request::RedirectMode::Error) {
            http_fetch_params = fetch_params;
            http_request = request;
        }
        // 2. Otherwise:
        else {
            // 1. Set httpRequest to a clone of request.
            // NOTE: Implementations are encouraged to avoid teeing request’s body’s stream when request’s body’s
            //       source is null as only a single body is needed in that case. E.g., when request’s body’s source
            //       is null, redirects and authentication will end up failing the fetch.
            http_request = request->clone(realm);

            // 2. Set httpFetchParams to a copy of fetchParams.
            // 3. Set httpFetchParams’s request to httpRequest.
            auto new_http_fetch_params = Infrastructure::FetchParams::create(vm, *http_request, fetch_params.timing_info());
            new_http_fetch_params->set_algorithms(fetch_params.algorithms());
            new_http_fetch_params->set_task_destination(fetch_params.task_destination());
            new_http_fetch_params->set_cross_origin_isolated_capability(fetch_params.cross_origin_isolated_capability());
            new_http_fetch_params->set_preloaded_response_candidate(fetch_params.preloaded_response_candidate());
            http_fetch_params = new_http_fetch_params;
        }

        // 3. Let includeCredentials be true if one of
        if (
            // - request’s credentials mode is "include"
            request->credentials_mode() == Infrastructure::Request::CredentialsMode::Include
            // - request’s credentials mode is "same-origin" and request’s response tainting is "basic"
            || (request->credentials_mode() == Infrastructure::Request::CredentialsMode::SameOrigin
                && request->response_tainting() == Infrastructure::Request::ResponseTainting::Basic)
            // is true; otherwise false.
        ) {
            include_credentials = IncludeCredentials::Yes;
        } else {
            include_credentials = IncludeCredentials::No;
        }

        // 4. If Cross-Origin-Embedder-Policy allows credentials with request returns false, then set
        //    includeCredentials to false.
        if (!request->cross_origin_embedder_policy_allows_credentials())
            include_credentials = IncludeCredentials::No;

        // 5. Let contentLength be httpRequest’s body’s length, if httpRequest’s body is non-null; otherwise null.
        auto content_length = http_request->body().has<JS::NonnullGCPtr<Infrastructure::Body>>()
            ? http_request->body().get<JS::NonnullGCPtr<Infrastructure::Body>>()->length()
            : Optional<u64> {};

        // 6. Let contentLengthHeaderValue be null.
        auto content_length_header_value = Optional<ByteBuffer> {};

        // 7. If httpRequest’s body is null and httpRequest’s method is `POST` or `PUT`, then set
        //    contentLengthHeaderValue to `0`.
        if (http_request->body().has<Empty>() && StringView { http_request->method() }.is_one_of("POST"sv, "PUT"sv))
            content_length_header_value = MUST(ByteBuffer::copy("0"sv.bytes()));

        // 8. If contentLength is non-null, then set contentLengthHeaderValue to contentLength, serialized and
        //    isomorphic encoded.
        if (content_length.has_value())
            content_length_header_value = MUST(ByteBuffer::copy(String::number(*content_length).bytes()));

        // 9. If contentLengthHeaderValue is non-null, then append (`Content-Length`, contentLengthHeaderValue) to
        //    httpRequest’s header list.
        if (content_length_header_value.has_value()) {
            auto header = Infrastructure::Header {
                .name = MUST(ByteBuffer::copy("Content-Length"sv.bytes())),
                .value = content_length_header_value.release_value(),
            };
            http_request->header_list()->append(move(header));
        }

        // 10. If contentLength is non-null and httpRequest’s keepalive is true, then:
        if (content_length.has_value() && http_request->keepalive()) {
            // 1. Let inflightKeepaliveBytes be 0.
            u64 inflight_keep_alive_bytes = 0;

            // 2. Let group be httpRequest’s client’s fetch group.
            auto& group = http_request->client()->fetch_group();

            // 3. Let inflightRecords be the set of fetch records in group whose request’s keepalive is true and done flag is unset.
            Vector<JS::NonnullGCPtr<Infrastructure::FetchRecord>> in_flight_records;
            for (auto const& fetch_record : group) {
                if (fetch_record->request()->keepalive() && !fetch_record->request()->done())
                    in_flight_records.append(fetch_record);
            }

            // 4. For each fetchRecord of inflightRecords:
            for (auto const& fetch_record : in_flight_records) {
                // 1. Let inflightRequest be fetchRecord’s request.
                auto const& in_flight_request = fetch_record->request();

                // 2. Increment inflightKeepaliveBytes by inflightRequest’s body’s length.
                inflight_keep_alive_bytes += in_flight_request->body().visit(
                    [](Empty) -> u64 { return 0; },
                    [](ByteBuffer const& buffer) -> u64 { return buffer.size(); },
                    [](JS::NonnullGCPtr<Infrastructure::Body> body) -> u64 {
                        return body->length().has_value() ? body->length().value() : 0;
                    });
            }

            // 5. If the sum of contentLength and inflightKeepaliveBytes is greater than 64 kibibytes, then return a network error.
            if ((content_length.value() + inflight_keep_alive_bytes) > keepalive_maximum_size)
                return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Keepalive request exceeded maximum allowed size of 64 KiB"sv));

            // NOTE: The above limit ensures that requests that are allowed to outlive the environment settings object
            //       and contain a body, have a bounded size and are not allowed to stay alive indefinitely.
        }

        // 11. If httpRequest’s referrer is a URL, then:
        if (http_request->referrer().has<URL::URL>()) {
            // 1. Let referrerValue be httpRequest’s referrer, serialized and isomorphic encoded.
            auto referrer_string = http_request->referrer().get<URL::URL>().serialize();
            auto referrer_value = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(referrer_string.bytes()));

            // 2. Append (`Referer`, referrerValue) to httpRequest’s header list.
            auto header = Infrastructure::Header {
                .name = MUST(ByteBuffer::copy("Referer"sv.bytes())),
                .value = move(referrer_value),
            };
            http_request->header_list()->append(move(header));
        }

        // 12. Append a request `Origin` header for httpRequest.
        http_request->add_origin_header();

        // 13. Append the Fetch metadata headers for httpRequest.
        append_fetch_metadata_headers_for_request(*http_request);

        // 14. FIXME If httpRequest’s initiator is "prefetch", then set a structured field value
        //     given (`Sec-Purpose`, the token prefetch) in httpRequest’s header list.

        // 15. If httpRequest’s header list does not contain `User-Agent`, then user agents should append
        //     (`User-Agent`, default `User-Agent` value) to httpRequest’s header list.
        if (!http_request->header_list()->contains("User-Agent"sv.bytes())) {
            auto header = Infrastructure::Header {
                .name = MUST(ByteBuffer::copy("User-Agent"sv.bytes())),
                .value = Infrastructure::default_user_agent_value(),
            };
            http_request->header_list()->append(move(header));
        }

        // 16. If httpRequest’s cache mode is "default" and httpRequest’s header list contains `If-Modified-Since`,
        //     `If-None-Match`, `If-Unmodified-Since`, `If-Match`, or `If-Range`, then set httpRequest’s cache mode to
        //     "no-store".
        if (http_request->cache_mode() == Infrastructure::Request::CacheMode::Default
            && (http_request->header_list()->contains("If-Modified-Since"sv.bytes())
                || http_request->header_list()->contains("If-None-Match"sv.bytes())
                || http_request->header_list()->contains("If-Unmodified-Since"sv.bytes())
                || http_request->header_list()->contains("If-Match"sv.bytes())
                || http_request->header_list()->contains("If-Range"sv.bytes()))) {
            http_request->set_cache_mode(Infrastructure::Request::CacheMode::NoStore);
        }

        // 17. If httpRequest’s cache mode is "no-cache", httpRequest’s prevent no-cache cache-control header
        //     modification flag is unset, and httpRequest’s header list does not contain `Cache-Control`, then append
        //     (`Cache-Control`, `max-age=0`) to httpRequest’s header list.
        if (http_request->cache_mode() == Infrastructure::Request::CacheMode::NoCache
            && !http_request->prevent_no_cache_cache_control_header_modification()
            && !http_request->header_list()->contains("Cache-Control"sv.bytes())) {
            auto header = Infrastructure::Header::from_string_pair("Cache-Control"sv, "max-age=0"sv);
            http_request->header_list()->append(move(header));
        }

        // 18. If httpRequest’s cache mode is "no-store" or "reload", then:
        if (http_request->cache_mode() == Infrastructure::Request::CacheMode::NoStore
            || http_request->cache_mode() == Infrastructure::Request::CacheMode::Reload) {
            // 1. If httpRequest’s header list does not contain `Pragma`, then append (`Pragma`, `no-cache`) to
            //    httpRequest’s header list.
            if (!http_request->header_list()->contains("Pragma"sv.bytes())) {
                auto header = Infrastructure::Header::from_string_pair("Pragma"sv, "no-cache"sv);
                http_request->header_list()->append(move(header));
            }

            // 2. If httpRequest’s header list does not contain `Cache-Control`, then append
            //    (`Cache-Control`, `no-cache`) to httpRequest’s header list.
            if (!http_request->header_list()->contains("Cache-Control"sv.bytes())) {
                auto header = Infrastructure::Header::from_string_pair("Cache-Control"sv, "no-cache"sv);
                http_request->header_list()->append(move(header));
            }
        }

        // 19. If httpRequest’s header list contains `Range`, then append (`Accept-Encoding`, `identity`) to
        //     httpRequest’s header list.
        // NOTE: This avoids a failure when handling content codings with a part of an encoded response.
        //       Additionally, many servers mistakenly ignore `Range` headers if a non-identity encoding is accepted.
        if (http_request->header_list()->contains("Range"sv.bytes())) {
            auto header = Infrastructure::Header::from_string_pair("Accept-Encoding"sv, "identity"sv);
            http_request->header_list()->append(move(header));
        }

        // 20. Modify httpRequest’s header list per HTTP. Do not append a given header if httpRequest’s header list
        //     contains that header’s name.
        // NOTE: It would be great if we could make this more normative somehow. At this point headers such as
        //       `Accept-Encoding`, `Connection`, `DNT`, and `Host`, are to be appended if necessary.
        //     `Accept`, `Accept-Charset`, and `Accept-Language` must not be included at this point.
        // NOTE: `Accept` and `Accept-Language` are already included (unless fetch() is used, which does not include
        //       the latter by default), and `Accept-Charset` is a waste of bytes. See HTTP header layer division for
        //       more details.
        if (ResourceLoader::the().enable_do_not_track() && !http_request->header_list()->contains("DNT"sv.bytes())) {
            auto header = Infrastructure::Header::from_string_pair("DNT"sv, "1"sv);
            http_request->header_list()->append(move(header));
        }

        // 21. If includeCredentials is true, then:
        if (include_credentials == IncludeCredentials::Yes) {
            // 1. If the user agent is not configured to block cookies for httpRequest (see section 7 of [COOKIES]),
            //    then:
            if (true) {
                // 1. Let cookies be the result of running the "cookie-string" algorithm (see section 5.4 of [COOKIES])
                //    with the user agent’s cookie store and httpRequest’s current URL.
                auto cookies = ([&] {
                    // FIXME: Getting to the page client reliably is way too complicated, and going via the document won't work in workers.
                    auto document = Bindings::host_defined_environment_settings_object(realm).responsible_document();
                    if (!document)
                        return String {};
                    return document->page().client().page_did_request_cookie(http_request->current_url(), Cookie::Source::Http);
                })();

                // 2. If cookies is not the empty string, then append (`Cookie`, cookies) to httpRequest’s header list.
                if (!cookies.is_empty()) {
                    auto header = Infrastructure::Header::from_string_pair("Cookie"sv, cookies);
                    http_request->header_list()->append(move(header));
                }
            }

            // 2. If httpRequest’s header list does not contain `Authorization`, then:
            if (!http_request->header_list()->contains("Authorization"sv.bytes())) {
                // 1. Let authorizationValue be null.
                auto authorization_value = Optional<String> {};

                // 2. If there’s an authentication entry for httpRequest and either httpRequest’s use-URL-credentials
                //    flag is unset or httpRequest’s current URL does not include credentials, then set
                //    authorizationValue to authentication entry.
                if (false // FIXME: "If there’s an authentication entry for httpRequest"
                    && (!http_request->use_url_credentials() || !http_request->current_url().includes_credentials())) {
                    // FIXME: "set authorizationValue to authentication entry."
                }
                // 3. Otherwise, if httpRequest’s current URL does include credentials and isAuthenticationFetch is
                //    true, set authorizationValue to httpRequest’s current URL, converted to an `Authorization` value.
                else if (http_request->current_url().includes_credentials() && is_authentication_fetch == IsAuthenticationFetch::Yes) {
                    auto const& url = http_request->current_url();
                    auto payload = MUST(String::formatted("{}:{}", URL::percent_decode(url.username()), URL::percent_decode(url.password())));
                    authorization_value = TRY_OR_THROW_OOM(vm, encode_base64(payload.bytes()));
                }

                // 4. If authorizationValue is non-null, then append (`Authorization`, authorizationValue) to
                //    httpRequest’s header list.
                if (authorization_value.has_value()) {
                    auto header = Infrastructure::Header::from_string_pair("Authorization"sv, *authorization_value);
                    http_request->header_list()->append(move(header));
                }
            }
        }

        // FIXME: 22. If there’s a proxy-authentication entry, use it as appropriate.
        // NOTE: This intentionally does not depend on httpRequest’s credentials mode.

        // 23. Set httpCache to the result of determining the HTTP cache partition, given httpRequest.
        http_cache = determine_the_http_cache_partition(*http_request);

        // 24. If httpCache is null, then set httpRequest’s cache mode to "no-store".
        if (!http_cache)
            http_request->set_cache_mode(Infrastructure::Request::CacheMode::NoStore);

        // 25. If httpRequest’s cache mode is neither "no-store" nor "reload", then:
        if (http_request->cache_mode() != Infrastructure::Request::CacheMode::NoStore
            && http_request->cache_mode() != Infrastructure::Request::CacheMode::Reload) {
            // 1. Set storedResponse to the result of selecting a response from the httpCache, possibly needing
            //    validation, as per the "Constructing Responses from Caches" chapter of HTTP Caching [HTTP-CACHING],
            //    if any.
            // NOTE: As mandated by HTTP, this still takes the `Vary` header into account.
            stored_response = http_cache->select_response(http_request->current_url(), http_request->method(), *http_request->header_list(), initial_set_of_stored_responses);
            // 2. If storedResponse is non-null, then:
            if (stored_response) {
                // 1. If cache mode is "default", storedResponse is a stale-while-revalidate response,
                //    and httpRequest’s client is non-null, then:
                if (http_request->cache_mode() == Infrastructure::Request::CacheMode::Default
                    && stored_response->is_stale_while_revalidate()
                    && http_request->client() != nullptr) {

                    // 1. Set response to storedResponse.
                    response = stored_response;

                    // 2. Set response’s cache state to "local".
                    response->set_cache_state(Infrastructure::Response::CacheState::Local);

                    // 3. Let revalidateRequest be a clone of request.
                    auto revalidate_request = request->clone(realm);

                    // 4. Set revalidateRequest’s cache mode set to "no-cache".
                    revalidate_request->set_cache_mode(Infrastructure::Request::CacheMode::NoCache);

                    // 5. Set revalidateRequest’s prevent no-cache cache-control header modification flag.
                    revalidate_request->set_prevent_no_cache_cache_control_header_modification(true);

                    // 6. Set revalidateRequest’s service-workers mode set to "none".
                    revalidate_request->set_service_workers_mode(Infrastructure::Request::ServiceWorkersMode::None);

                    // 7. In parallel, run main fetch given a new fetch params whose request is revalidateRequest.
                    Platform::EventLoopPlugin::the().deferred_invoke([&vm, &realm, revalidate_request, fetch_params = JS::NonnullGCPtr(fetch_params)] {
                        (void)main_fetch(realm, Infrastructure::FetchParams::create(vm, revalidate_request, fetch_params->timing_info()));
                    });
                }
                // 2. Otherwise:
                else {
                    // 1. If storedResponse is a stale response, then set the revalidatingFlag.
                    if (stored_response->is_stale())
                        revalidating_flag->set_value(true);

                    // 2. If the revalidatingFlag is set and httpRequest’s cache mode is neither "force-cache" nor "only-if-cached", then:
                    if (revalidating_flag->value()
                        && http_request->cache_mode() != Infrastructure::Request::CacheMode::ForceCache
                        && http_request->cache_mode() != Infrastructure::Request::CacheMode::OnlyIfCached) {

                        // 1. If storedResponse’s header list contains `ETag`, then append (`If-None-Match`, `ETag`'s value) to httpRequest’s header list.
                        if (auto etag = stored_response->header_list()->get("ETag"sv.bytes()); etag.has_value()) {
                            http_request->header_list()->append(Infrastructure::Header::from_string_pair("If-None-Match"sv, *etag));
                        }

                        // 2. If storedResponse’s header list contains `Last-Modified`, then append (`If-Modified-Since`, `Last-Modified`'s value) to httpRequest’s header list.
                        if (auto last_modified = stored_response->header_list()->get("Last-Modified"sv.bytes()); last_modified.has_value()) {
                            http_request->header_list()->append(Infrastructure::Header::from_string_pair("If-Modified-Since"sv, *last_modified));
                        }
                    }
                    // 3. Otherwise, set response to storedResponse and set response’s cache state to "local".
                    else {
                        response = stored_response;
                        response->set_cache_state(Infrastructure::Response::CacheState::Local);
                    }
                }
            }
        }
    }

    // 9. If aborted, then return the appropriate network error for fetchParams.
    if (aborted)
        return PendingResponse::create(vm, request, Infrastructure::Response::appropriate_network_error(vm, fetch_params));

    JS::GCPtr<PendingResponse> pending_forward_response;

    // 10. If response is null, then:
    if (!response) {
        // 1. If httpRequest’s cache mode is "only-if-cached", then return a network error.
        if (http_request->cache_mode() == Infrastructure::Request::CacheMode::OnlyIfCached)
            return PendingResponse::create(vm, request, Infrastructure::Response::network_error(vm, "Request with 'only-if-cached' cache mode doesn't have a cached response"sv));

        // 2. Let forwardResponse be the result of running HTTP-network fetch given httpFetchParams, includeCredentials,
        //    and isNewConnectionFetch.
        pending_forward_response = TRY(nonstandard_resource_loader_file_or_http_network_fetch(realm, *http_fetch_params, include_credentials, is_new_connection_fetch));
    } else {
        pending_forward_response = PendingResponse::create(vm, request, Infrastructure::Response::create(vm));
    }

    auto returned_pending_response = PendingResponse::create(vm, request);

    pending_forward_response->when_loaded([&realm, &vm, &fetch_params, request, response, stored_response, initial_set_of_stored_responses, http_request, returned_pending_response, is_authentication_fetch, is_new_connection_fetch, revalidating_flag, include_credentials, response_was_null = !response, http_cache](JS::NonnullGCPtr<Infrastructure::Response> resolved_forward_response) mutable {
        dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP-network-or-cache fetch' pending_forward_response load callback");
        if (response_was_null) {
            auto forward_response = resolved_forward_response;

            // NOTE: TRACE is omitted as it is a forbidden method in Fetch.
            auto method_is_unsafe = !(StringView { http_request->method() }.is_one_of("GET"sv, "HEAD"sv, "OPTIONS"sv));

            // 3. If httpRequest’s method is unsafe and forwardResponse’s status is in the range 200 to 399, inclusive,
            //    invalidate appropriate stored responses in httpCache, as per the "Invalidation" chapter of HTTP
            //    Caching, and set storedResponse to null.
            if (method_is_unsafe && forward_response->status() >= 200 && forward_response->status() <= 399) {
                // FIXME: "invalidate appropriate stored responses in httpCache, as per the "Invalidation" chapter of HTTP Caching"
                stored_response = nullptr;
            }

            // 4. If the revalidatingFlag is set and forwardResponse’s status is 304, then:
            if (revalidating_flag->value() && forward_response->status() == 304) {
                dbgln("\033[34;1mHTTP CACHE REVALIDATE (304)\033[0m {}", http_request->current_url());
                // 1. Update storedResponse’s header list using forwardResponse’s header list, as per the "Freshening
                //    Stored Responses upon Validation" chapter of HTTP Caching.
                // NOTE: This updates the stored response in cache as well.
                http_cache->freshen_stored_responses_upon_validation(*forward_response, initial_set_of_stored_responses);

                // 2. Set response to storedResponse.
                response = stored_response;

                // 3. Set response’s cache state to "validated".
                if (response)
                    response->set_cache_state(Infrastructure::Response::CacheState::Validated);
            }

            // 5. If response is null, then:
            if (!response) {
                // 1. Set response to forwardResponse.
                response = forward_response;

                // 2. Store httpRequest and forwardResponse in httpCache, as per the "Storing Responses in Caches" chapter of HTTP Caching.
                // NOTE: If forwardResponse is a network error, this effectively caches the network error, which is
                //       sometimes known as "negative caching".
                // NOTE: The associated body info is stored in the cache alongside the response.
                if (http_cache)
                    http_cache->store_response(realm, *http_request, *forward_response);
            }
        }

        // 11. Set response’s URL list to a clone of httpRequest’s URL list.
        response->set_url_list(http_request->url_list());

        // 12. If httpRequest’s header list contains `Range`, then set response’s range-requested flag.
        if (http_request->header_list()->contains("Range"sv.bytes()))
            response->set_range_requested(true);

        // 13. Set response’s request-includes-credentials to includeCredentials.
        response->set_request_includes_credentials(include_credentials == IncludeCredentials::Yes);

        auto inner_pending_response = PendingResponse::create(vm, request, *response);

        // 14. If response’s status is 401, httpRequest’s response tainting is not "cors", includeCredentials is true,
        //     and request’s window is an environment settings object, then:
        if (response->status() == 401
            && http_request->response_tainting() != Infrastructure::Request::ResponseTainting::CORS
            && include_credentials == IncludeCredentials::Yes
            && request->window().has<JS::GCPtr<HTML::EnvironmentSettingsObject>>()
            // AD-HOC: Require at least one WWW-Authenticate header to be set before automatically retrying an authenticated
            //         request (see rule 1 below). See: https://github.com/whatwg/fetch/issues/1766
            && request->header_list()->contains("WWW-Authenticate"sv.bytes())) {
            // 1. Needs testing: multiple `WWW-Authenticate` headers, missing, parsing issues.
            // (Red box in the spec, no-op)

            // 2. If request’s body is non-null, then:
            if (!request->body().has<Empty>()) {
                // 1. If request’s body’s source is null, then return a network error.
                if (request->body().get<JS::NonnullGCPtr<Infrastructure::Body>>()->source().has<Empty>()) {
                    returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "Request has body but no body source"_string));
                    return;
                }

                // 2. Set request’s body to the body of the result of safely extracting request’s body’s source.
                auto const& source = request->body().get<JS::NonnullGCPtr<Infrastructure::Body>>()->source();
                // NOTE: BodyInitOrReadableBytes is a superset of Body::SourceType
                auto converted_source = source.has<ByteBuffer>()
                    ? BodyInitOrReadableBytes { source.get<ByteBuffer>() }
                    : BodyInitOrReadableBytes { source.get<JS::Handle<FileAPI::Blob>>() };
                auto [body, _] = TRY_OR_IGNORE(safely_extract_body(realm, converted_source));
                request->set_body(move(body));
            }

            // 3. If request’s use-URL-credentials flag is unset or isAuthenticationFetch is true, then:
            if (!request->use_url_credentials() || is_authentication_fetch == IsAuthenticationFetch::Yes) {
                // 1. If fetchParams is canceled, then return the appropriate network error for fetchParams.
                if (fetch_params.is_canceled()) {
                    returned_pending_response->resolve(Infrastructure::Response::appropriate_network_error(vm, fetch_params));
                    return;
                }

                // FIXME: 2. Let username and password be the result of prompting the end user for a username and password,
                //           respectively, in request’s window.
                dbgln("Fetch: Username/password prompt is not implemented, using empty strings. This request will probably fail.");
                auto username = ByteString::empty();
                auto password = ByteString::empty();

                // 3. Set the username given request’s current URL and username.
                request->current_url().set_username(username);

                // 4. Set the password given request’s current URL and password.
                request->current_url().set_password(password);
            }

            // 4. Set response to the result of running HTTP-network-or-cache fetch given fetchParams and true.
            inner_pending_response = TRY_OR_IGNORE(http_network_or_cache_fetch(realm, fetch_params, IsAuthenticationFetch::Yes));
        }

        inner_pending_response->when_loaded([&realm, &vm, &fetch_params, request, returned_pending_response, is_authentication_fetch, is_new_connection_fetch](JS::NonnullGCPtr<Infrastructure::Response> response) {
            dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'HTTP network-or-cache fetch' inner_pending_response load callback");
            // 15. If response’s status is 407, then:
            if (response->status() == 407) {
                // 1. If request’s window is "no-window", then return a network error.
                if (request->window().has<Infrastructure::Request::Window>()
                    && request->window().get<Infrastructure::Request::Window>() == Infrastructure::Request::Window::NoWindow) {
                    returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "Request requires proxy authentication but has 'no-window' set"_string));
                    return;
                }

                // 2. Needs testing: multiple `Proxy-Authenticate` headers, missing, parsing issues.
                // (Red box in the spec, no-op)

                // 3. If fetchParams is canceled, then return the appropriate network error for fetchParams.
                if (fetch_params.is_canceled()) {
                    returned_pending_response->resolve(Infrastructure::Response::appropriate_network_error(vm, fetch_params));
                    return;
                }

                // FIXME: 4. Prompt the end user as appropriate in request’s window and store the result as a
                //           proxy-authentication entry.
                // NOTE: Remaining details surrounding proxy authentication are defined by HTTP.

                // FIXME: 5. Set response to the result of running HTTP-network-or-cache fetch given fetchParams.
                // (Doing this without step 4 would potentially lead to an infinite request cycle.)
            }

            auto inner_pending_response = PendingResponse::create(vm, request, *response);

            // 16. If all of the following are true
            if (
                // - response’s status is 421
                response->status() == 421
                // - isNewConnectionFetch is false
                && is_new_connection_fetch == IsNewConnectionFetch::No
                // - request’s body is null, or request’s body is non-null and request’s body’s source is non-null
                && (request->body().has<Empty>() || !request->body().get<JS::NonnullGCPtr<Infrastructure::Body>>()->source().has<Empty>())
                // then:
            ) {
                // 1. If fetchParams is canceled, then return the appropriate network error for fetchParams.
                if (fetch_params.is_canceled()) {
                    returned_pending_response->resolve(Infrastructure::Response::appropriate_network_error(vm, fetch_params));
                    return;
                }
                // 2. Set response to the result of running HTTP-network-or-cache fetch given fetchParams,
                //    isAuthenticationFetch, and true.
                inner_pending_response = TRY_OR_IGNORE(http_network_or_cache_fetch(realm, fetch_params, is_authentication_fetch, IsNewConnectionFetch::Yes));
            }

            inner_pending_response->when_loaded([returned_pending_response, is_authentication_fetch](JS::NonnullGCPtr<Infrastructure::Response> response) {
                // 17. If isAuthenticationFetch is true, then create an authentication entry for request and the given
                //     realm.
                if (is_authentication_fetch == IsAuthenticationFetch::Yes) {
                    // FIXME: "create an authentication entry for request and the given realm"
                }

                returned_pending_response->resolve(response);
            });
        });
    });

    // 18. Return response.
    // NOTE: Typically response’s body’s stream is still being enqueued to after returning.
    return returned_pending_response;
}

#if defined(WEB_FETCH_DEBUG)
static void log_load_request(auto const& load_request)
{
    dbgln("Fetch: Invoking ResourceLoader");
    dbgln("> {} {} HTTP/1.1", load_request.method(), load_request.url());
    for (auto const& [name, value] : load_request.headers())
        dbgln("> {}: {}", name, value);
    dbgln(">");
    for (auto line : StringView { load_request.body() }.split_view('\n', SplitBehavior::KeepEmpty))
        dbgln("> {}", line);
}

static void log_response(auto const& status_code, auto const& headers, auto const& data)
{
    dbgln("< HTTP/1.1 {}", status_code.value_or(0));
    for (auto const& [name, value] : headers.headers())
        dbgln("< {}: {}", name, value);
    dbgln("<");
    for (auto line : StringView { data }.split_view('\n', SplitBehavior::KeepEmpty))
        dbgln("< {}", line);
}
#endif

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// Drop-in replacement for 'HTTP-network fetch', but obviously non-standard :^)
// It also handles file:// URLs since those can also go through ResourceLoader.
WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> nonstandard_resource_loader_file_or_http_network_fetch(JS::Realm& realm, Infrastructure::FetchParams const& fetch_params, IncludeCredentials include_credentials, IsNewConnectionFetch is_new_connection_fetch)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'non-standard HTTP-network fetch' with: fetch_params @ {}", &fetch_params);

    auto& vm = realm.vm();

    (void)include_credentials;
    (void)is_new_connection_fetch;

    auto request = fetch_params.request();

    auto& page = Bindings::host_defined_page(realm);

    // NOTE: Using LoadRequest::create_for_url_on_page here will unconditionally add cookies as long as there's a page available.
    //       However, it is up to http_network_or_cache_fetch to determine if cookies should be added to the request.
    LoadRequest load_request;
    load_request.set_url(request->current_url());
    load_request.set_page(page);
    load_request.set_method(ByteString::copy(request->method()));

    for (auto const& header : *request->header_list())
        load_request.set_header(ByteString::copy(header.name), ByteString::copy(header.value));

    if (auto const* body = request->body().get_pointer<JS::NonnullGCPtr<Infrastructure::Body>>()) {
        TRY((*body)->source().visit(
            [&](ByteBuffer const& byte_buffer) -> WebIDL::ExceptionOr<void> {
                load_request.set_body(TRY_OR_THROW_OOM(vm, ByteBuffer::copy(byte_buffer)));
                return {};
            },
            [&](JS::Handle<FileAPI::Blob> const& blob_handle) -> WebIDL::ExceptionOr<void> {
                load_request.set_body(TRY_OR_THROW_OOM(vm, ByteBuffer::copy(blob_handle->raw_bytes())));
                return {};
            },
            [](Empty) -> WebIDL::ExceptionOr<void> {
                return {};
            }));
    }

    auto pending_response = PendingResponse::create(vm, request);

    if constexpr (WEB_FETCH_DEBUG) {
        dbgln("Fetch: Invoking ResourceLoader");
        log_load_request(load_request);
    }

    // FIXME: This check should be removed and all HTTP requests should go through the `ResourceLoader::load_unbuffered`
    //        path. The buffer option should then be supplied to the steps below that allow us to buffer data up to a
    //        user-agent-defined limit (or not). However, we will need to fully use stream operations throughout the
    //        fetch process to enable this (e.g. Body::fully_read must use streams for this to work).
    if (request->buffer_policy() == Infrastructure::Request::BufferPolicy::DoNotBufferResponse) {
        HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 12. Let stream be a new ReadableStream.
        auto stream = realm.heap().allocate<Streams::ReadableStream>(realm, realm);
        auto fetched_data_receiver = realm.heap().allocate<FetchedDataReceiver>(realm, fetch_params, stream);

        // 10. Let pullAlgorithm be the followings steps:
        auto pull_algorithm = JS::create_heap_function(realm.heap(), [&realm, fetched_data_receiver]() {
            // 1. Let promise be a new promise.
            auto promise = WebIDL::create_promise(realm);

            // 2. Run the following steps in parallel:
            // NOTE: This is handled by FetchedDataReceiver.
            fetched_data_receiver->set_pending_promise(promise);

            // 3. Return promise.
            return promise;
        });

        // 11. Let cancelAlgorithm be an algorithm that aborts fetchParams’s controller with reason, given reason.
        auto cancel_algorithm = JS::create_heap_function(realm.heap(), [&realm, &fetch_params](JS::Value reason) {
            fetch_params.controller()->abort(realm, reason);
            return WebIDL::create_resolved_promise(realm, JS::js_undefined());
        });

        // 13. Set up stream with byte reading support with pullAlgorithm set to pullAlgorithm, cancelAlgorithm set to cancelAlgorithm.
        Streams::set_up_readable_stream_controller_with_byte_reading_support(stream, pull_algorithm, cancel_algorithm);

        auto on_headers_received = [&vm, request, pending_response, stream](auto const& response_headers, Optional<u32> status_code) {
            if (pending_response->is_resolved()) {
                // RequestServer will send us the response headers twice, the second time being for HTTP trailers. This
                // fetch algorithm is not interested in trailers, so just drop them here.
                return;
            }

            auto response = Infrastructure::Response::create(vm);
            response->set_status(status_code.value_or(200));
            // FIXME: Set response status message

            if constexpr (WEB_FETCH_DEBUG) {
                dbgln("Fetch: ResourceLoader load for '{}' {}: (status {})",
                    request->url(),
                    Infrastructure::is_ok_status(response->status()) ? "complete"sv : "failed"sv,
                    response->status());
                log_response(status_code, response_headers, ReadonlyBytes {});
            }

            for (auto const& [name, value] : response_headers.headers()) {
                auto header = Infrastructure::Header::from_string_pair(name, value);
                response->header_list()->append(move(header));
            }

            // 14. Set response’s body to a new body whose stream is stream.
            response->set_body(Infrastructure::Body::create(vm, stream));

            // 17. Return response.
            // NOTE: Typically response’s body’s stream is still being enqueued to after returning.
            pending_response->resolve(response);
        };

        // 16. Run these steps in parallel:
        //    FIXME: 1. Run these steps, but abort when fetchParams is canceled:
        auto on_data_received = [fetched_data_receiver](auto bytes) {
            // 1. If one or more bytes have been transmitted from response’s message body, then:
            if (!bytes.is_empty()) {
                // 1. Let bytes be the transmitted bytes.

                // FIXME: 2. Let codings be the result of extracting header list values given `Content-Encoding` and response’s header list.
                // FIXME: 3. Increase response’s body info’s encoded size by bytes’s length.
                // FIXME: 4. Set bytes to the result of handling content codings given codings and bytes.
                // FIXME: 5. Increase response’s body info’s decoded size by bytes’s length.
                // FIXME: 6. If bytes is failure, then terminate fetchParams’s controller.

                // 7. Append bytes to buffer.
                fetched_data_receiver->on_data_received(bytes);

                // FIXME: 8. If the size of buffer is larger than an upper limit chosen by the user agent, ask the user agent
                //           to suspend the ongoing fetch.
            }
        };

        auto on_complete = [&vm, &realm, pending_response, stream](auto success, auto error_message) {
            HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 16.1.1.2. Otherwise, if the bytes transmission for response’s message body is done normally and stream is readable,
            //           then close stream, and abort these in-parallel steps.
            if (success) {
                if (stream->is_readable())
                    stream->close();
            }
            // 16.1.2.2. Otherwise, if stream is readable, error stream with a TypeError.
            else {
                auto error = MUST(String::formatted("Load failed: {}", error_message));

                if (stream->is_readable())
                    stream->error(JS::TypeError::create(realm, error));

                if (!pending_response->is_resolved())
                    pending_response->resolve(Infrastructure::Response::network_error(vm, error));
            }
        };

        ResourceLoader::the().load_unbuffered(load_request, move(on_headers_received), move(on_data_received), move(on_complete));
    } else {
        auto on_load_success = [&realm, &vm, request, pending_response](auto data, auto& response_headers, auto status_code) {
            dbgln_if(WEB_FETCH_DEBUG, "Fetch: ResourceLoader load for '{}' complete", request->url());
            if constexpr (WEB_FETCH_DEBUG)
                log_response(status_code, response_headers, data);
            auto [body, _] = TRY_OR_IGNORE(extract_body(realm, data));
            auto response = Infrastructure::Response::create(vm);
            response->set_status(status_code.value_or(200));
            response->set_body(move(body));
            for (auto const& [name, value] : response_headers.headers()) {
                auto header = Infrastructure::Header::from_string_pair(name, value);
                response->header_list()->append(move(header));
            }
            // FIXME: Set response status message
            pending_response->resolve(response);
        };

        auto on_load_error = [&realm, &vm, request, pending_response](auto& error, auto status_code, auto data, auto& response_headers) {
            dbgln_if(WEB_FETCH_DEBUG, "Fetch: ResourceLoader load for '{}' failed: {} (status {})", request->url(), error, status_code.value_or(0));
            if constexpr (WEB_FETCH_DEBUG)
                log_response(status_code, response_headers, data);
            auto response = Infrastructure::Response::create(vm);
            // FIXME: This is ugly, ResourceLoader should tell us.
            if (status_code.value_or(0) == 0) {
                response = Infrastructure::Response::network_error(vm, TRY_OR_IGNORE(String::from_byte_string(error)));
            } else {
                response->set_type(Infrastructure::Response::Type::Error);
                response->set_status(status_code.value_or(400));
                auto [body, _] = TRY_OR_IGNORE(extract_body(realm, data));
                response->set_body(move(body));
                for (auto const& [name, value] : response_headers.headers()) {
                    auto header = Infrastructure::Header::from_string_pair(name, value);
                    response->header_list()->append(move(header));
                }
                // FIXME: Set response status message
            }
            pending_response->resolve(response);
        };

        ResourceLoader::the().load(load_request, move(on_load_success), move(on_load_error));
    }

    return pending_response;
}

// https://fetch.spec.whatwg.org/#cors-preflight-fetch-0
WebIDL::ExceptionOr<JS::NonnullGCPtr<PendingResponse>> cors_preflight_fetch(JS::Realm& realm, Infrastructure::Request& request)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'CORS-preflight fetch' with request @ {}", &request);

    auto& vm = realm.vm();

    // 1. Let preflight be a new request whose method is `OPTIONS`, URL list is a clone of request’s URL list, initiator is
    //    request’s initiator, destination is request’s destination, origin is request’s origin, referrer is request’s referrer,
    //    referrer policy is request’s referrer policy, mode is "cors", and response tainting is "cors".
    auto preflight = Fetch::Infrastructure::Request::create(vm);
    preflight->set_method(TRY_OR_THROW_OOM(vm, ByteBuffer::copy("OPTIONS"sv.bytes())));
    preflight->set_url_list(request.url_list());
    preflight->set_initiator(request.initiator());
    preflight->set_destination(request.destination());
    preflight->set_origin(request.origin());
    preflight->set_referrer(request.referrer());
    preflight->set_referrer_policy(request.referrer_policy());
    preflight->set_mode(Infrastructure::Request::Mode::CORS);
    preflight->set_response_tainting(Infrastructure::Request::ResponseTainting::CORS);

    // 2. Append (`Accept`, `*/*`) to preflight’s header list.
    auto temp_header = Infrastructure::Header::from_string_pair("Accept"sv, "*/*"sv);
    preflight->header_list()->append(move(temp_header));

    // 3. Append (`Access-Control-Request-Method`, request’s method) to preflight’s header list.
    temp_header = Infrastructure::Header::from_string_pair("Access-Control-Request-Method"sv, request.method());
    preflight->header_list()->append(move(temp_header));

    // 4. Let headers be the CORS-unsafe request-header names with request’s header list.
    auto headers = Infrastructure::get_cors_unsafe_header_names(request.header_list());

    // 5. If headers is not empty, then:
    if (!headers.is_empty()) {
        // 1. Let value be the items in headers separated from each other by `,`.
        // NOTE: This intentionally does not use combine, as 0x20 following 0x2C is not the way this was implemented,
        //       for better or worse.
        ByteBuffer value;

        bool first = true;
        for (auto const& header : headers) {
            if (!first)
                TRY_OR_THROW_OOM(vm, value.try_append(','));
            TRY_OR_THROW_OOM(vm, value.try_append(header));
            first = false;
        }

        // 2. Append (`Access-Control-Request-Headers`, value) to preflight’s header list.
        temp_header = Infrastructure::Header {
            .name = TRY_OR_THROW_OOM(vm, ByteBuffer::copy("Access-Control-Request-Headers"sv.bytes())),
            .value = move(value),
        };
        preflight->header_list()->append(move(temp_header));
    }

    // 6. Let response be the result of running HTTP-network-or-cache fetch given a new fetch params whose request is preflight.
    // FIXME: The spec doesn't say anything about timing_info here, but FetchParams requires a non-null FetchTimingInfo object.
    auto timing_info = Infrastructure::FetchTimingInfo::create(vm);
    auto fetch_params = Infrastructure::FetchParams::create(vm, preflight, timing_info);

    auto returned_pending_response = PendingResponse::create(vm, request);

    auto preflight_response = TRY(http_network_or_cache_fetch(realm, fetch_params));

    preflight_response->when_loaded([&vm, &request, returned_pending_response](JS::NonnullGCPtr<Infrastructure::Response> response) {
        dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'CORS-preflight fetch' preflight_response load callback");

        // 7. If a CORS check for request and response returns success and response’s status is an ok status, then:
        // NOTE: The CORS check is done on request rather than preflight to ensure the correct credentials mode is used.
        if (cors_check(request, response) && Infrastructure::is_ok_status(response->status())) {
            // 1. Let methods be the result of extracting header list values given `Access-Control-Allow-Methods` and response’s header list.
            auto methods_or_failure = Infrastructure::extract_header_list_values("Access-Control-Allow-Methods"sv.bytes(), response->header_list());

            // 2. Let headerNames be the result of extracting header list values given `Access-Control-Allow-Headers` and
            //    response’s header list.
            auto header_names_or_failure = Infrastructure::extract_header_list_values("Access-Control-Allow-Headers"sv.bytes(), response->header_list());

            // 3. If either methods or headerNames is failure, return a network error.
            if (methods_or_failure.has<Infrastructure::ExtractHeaderParseFailure>()) {
                returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "The Access-Control-Allow-Methods in the CORS-preflight response is syntactically invalid"_string));
                return;
            }

            if (header_names_or_failure.has<Infrastructure::ExtractHeaderParseFailure>()) {
                returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "The Access-Control-Allow-Headers in the CORS-preflight response is syntactically invalid"_string));
                return;
            }

            // NOTE: We treat "methods_or_failure" being `Empty` as empty Vector here.
            auto methods = methods_or_failure.has<Vector<ByteBuffer>>() ? methods_or_failure.get<Vector<ByteBuffer>>() : Vector<ByteBuffer> {};

            // NOTE: We treat "header_names_or_failure" being `Empty` as empty Vector here.
            auto header_names = header_names_or_failure.has<Vector<ByteBuffer>>() ? header_names_or_failure.get<Vector<ByteBuffer>>() : Vector<ByteBuffer> {};

            // 4. If methods is null and request’s use-CORS-preflight flag is set, then set methods to a new list containing request’s method.
            // NOTE: This ensures that a CORS-preflight fetch that happened due to request’s use-CORS-preflight flag being set is cached.
            if (methods.is_empty() && request.use_cors_preflight())
                methods = Vector { TRY_OR_IGNORE(ByteBuffer::copy(request.method())) };

            // 5. If request’s method is not in methods, request’s method is not a CORS-safelisted method, and request’s credentials mode
            //    is "include" or methods does not contain `*`, then return a network error.
            if (!methods.contains_slow(request.method()) && !Infrastructure::is_cors_safelisted_method(request.method())) {
                if (request.credentials_mode() == Infrastructure::Request::CredentialsMode::Include) {
                    returned_pending_response->resolve(Infrastructure::Response::network_error(vm, TRY_OR_IGNORE(String::formatted("Non-CORS-safelisted method '{}' not found in the CORS-preflight response's Access-Control-Allow-Methods header (the header may be missing). '*' is not allowed as the main request includes credentials."sv, StringView { request.method() }))));
                    return;
                }

                if (!methods.contains_slow("*"sv.bytes())) {
                    returned_pending_response->resolve(Infrastructure::Response::network_error(vm, TRY_OR_IGNORE(String::formatted("Non-CORS-safelisted method '{}' not found in the CORS-preflight response's Access-Control-Allow-Methods header and there was no '*' entry. The header may be missing."sv, StringView { request.method() }))));
                    return;
                }
            }

            // 6. If one of request’s header list’s names is a CORS non-wildcard request-header name and is not a byte-case-insensitive match
            //    for an item in headerNames, then return a network error.
            for (auto const& header : *request.header_list()) {
                if (Infrastructure::is_cors_non_wildcard_request_header_name(header.name)) {
                    bool is_in_header_names = false;

                    for (auto const& allowed_header_name : header_names) {
                        if (StringView { allowed_header_name }.equals_ignoring_ascii_case(header.name)) {
                            is_in_header_names = true;
                            break;
                        }
                    }

                    if (!is_in_header_names) {
                        returned_pending_response->resolve(Infrastructure::Response::network_error(vm, TRY_OR_IGNORE(String::formatted("Main request contains the header '{}' that is not specified in the CORS-preflight response's Access-Control-Allow-Headers header (the header may be missing). '*' does not capture this header."sv, StringView { header.name }))));
                        return;
                    }
                }
            }

            // 7. For each unsafeName of the CORS-unsafe request-header names with request’s header list, if unsafeName is not a
            //    byte-case-insensitive match for an item in headerNames and request’s credentials mode is "include" or headerNames
            //    does not contain `*`, return a network error.
            auto unsafe_names = Infrastructure::get_cors_unsafe_header_names(request.header_list());
            for (auto const& unsafe_name : unsafe_names) {
                bool is_in_header_names = false;

                for (auto const& header_name : header_names) {
                    if (StringView { unsafe_name }.equals_ignoring_ascii_case(header_name)) {
                        is_in_header_names = true;
                        break;
                    }
                }

                if (!is_in_header_names) {
                    if (request.credentials_mode() == Infrastructure::Request::CredentialsMode::Include) {
                        returned_pending_response->resolve(Infrastructure::Response::network_error(vm, TRY_OR_IGNORE(String::formatted("CORS-unsafe request-header '{}' not found in the CORS-preflight response's Access-Control-Allow-Headers header (the header may be missing). '*' is not allowed as the main request includes credentials."sv, StringView { unsafe_name }))));
                        return;
                    }

                    if (!header_names.contains_slow("*"sv.bytes())) {
                        returned_pending_response->resolve(Infrastructure::Response::network_error(vm, TRY_OR_IGNORE(String::formatted("CORS-unsafe request-header '{}' not found in the CORS-preflight response's Access-Control-Allow-Headers header and there was no '*' entry. The header may be missing."sv, StringView { unsafe_name }))));
                        return;
                    }
                }
            }

            // FIXME: 8. Let max-age be the result of extracting header list values given `Access-Control-Max-Age` and response’s header list.
            // FIXME: 9. If max-age is failure or null, then set max-age to 5.
            // FIXME: 10. If max-age is greater than an imposed limit on max-age, then set max-age to the imposed limit.

            // 11. If the user agent does not provide for a cache, then return response.
            // NOTE: Since we don't currently have a cache, this is always true.
            returned_pending_response->resolve(response);
            return;

            // FIXME: 12. For each method in methods for which there is a method cache entry match using request, set matching entry’s max-age
            //            to max-age.
            // FIXME: 13. For each method in methods for which there is no method cache entry match using request, create a new cache entry
            //            with request, max-age, method, and null.
            // FIXME: 14. For each headerName in headerNames for which there is a header-name cache entry match using request, set matching
            //            entry’s max-age to max-age.
            // FIXME: 15. For each headerName in headerNames for which there is no header-name cache entry match using request, create a
            //            new cache entry with request, max-age, null, and headerName.
            // FIXME: 16. Return response.
        }

        // 8. Otherwise, return a network error.
        returned_pending_response->resolve(Infrastructure::Response::network_error(vm, "CORS-preflight check failed"_string));
    });

    return returned_pending_response;
}

// https://w3c.github.io/webappsec-fetch-metadata/#abstract-opdef-set-dest
void set_sec_fetch_dest_header(Infrastructure::Request& request)
{
    // 1. Assert: r’s url is a potentially trustworthy URL.
    VERIFY(SecureContexts::is_url_potentially_trustworthy(request.url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy);

    // 2. Let header be a Structured Header whose value is a token.
    // FIXME: This is handled below, as Serenity doesn't have APIs for RFC 8941.

    // 3. If r’s destination is the empty string, set header’s value to the string "empty". Otherwise, set header’s value to r’s destination.
    ByteBuffer header_value;
    if (!request.destination().has_value()) {
        header_value = MUST(ByteBuffer::copy("empty"sv.bytes()));
    } else {
        header_value = MUST(ByteBuffer::copy(Infrastructure::request_destination_to_string(request.destination().value()).bytes()));
    }

    // 4. Set a structured field value `Sec-Fetch-Dest`/header in r’s header list.
    auto header = Infrastructure::Header {
        .name = MUST(ByteBuffer::copy("Sec-Fetch-Dest"sv.bytes())),
        .value = move(header_value),
    };
    request.header_list()->append(move(header));
}

// https://w3c.github.io/webappsec-fetch-metadata/#abstract-opdef-set-dest
void set_sec_fetch_mode_header(Infrastructure::Request& request)
{
    // 1. Assert: r’s url is a potentially trustworthy URL.
    VERIFY(SecureContexts::is_url_potentially_trustworthy(request.url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy);

    // 2. Let header be a Structured Header whose value is a token.
    // FIXME: This is handled below, as Serenity doesn't have APIs for RFC 8941.

    // 3. Set header’s value to r’s mode.
    auto header_value = MUST(ByteBuffer::copy(Infrastructure::request_mode_to_string(request.mode()).bytes()));

    // 4. Set a structured field value `Sec-Fetch-Mode`/header in r’s header list.
    auto header = Infrastructure::Header {
        .name = MUST(ByteBuffer::copy("Sec-Fetch-Mode"sv.bytes())),
        .value = move(header_value),
    };
    request.header_list()->append(move(header));
}

// https://w3c.github.io/webappsec-fetch-metadata/#abstract-opdef-set-site
void set_sec_fetch_site_header(Infrastructure::Request& request)
{
    // 1. Assert: r’s url is a potentially trustworthy URL.
    VERIFY(SecureContexts::is_url_potentially_trustworthy(request.url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy);

    // 2. Let header be a Structured Header whose value is a token.
    // FIXME: This is handled below, as Serenity doesn't have APIs for RFC 8941.

    // 3. Set header’s value to same-origin.
    auto header_value = "same-origin"sv;

    // FIXME: 4. If r is a navigation request that was explicitly caused by a user’s interaction with the user agent (by typing an address
    //           into the user agent directly, for example, or by clicking a bookmark, etc.), then set header’s value to none.

    // 5. If header’s value is not none, then for each url in r’s url list:
    if (!header_value.equals_ignoring_ascii_case("none"sv)) {
        for (auto& url : request.url_list()) {
            // 1. If url is same origin with r’s origin, continue.
            if (url.origin().is_same_origin(request.current_url().origin()))
                continue;

            // 2. Set header’s value to cross-site.
            header_value = "cross-site"sv;

            // FIXME: 3. If r’s origin is not same site with url’s origin, then break.

            // FIXME: 4. Set header’s value to same-site.
        }
    }

    // 6. Set a structured field value `Sec-Fetch-Site`/header in r’s header list.
    auto header = Infrastructure::Header {
        .name = MUST(ByteBuffer::copy("Sec-Fetch-Site"sv.bytes())),
        .value = MUST(ByteBuffer::copy(header_value.bytes())),
    };
    request.header_list()->append(move(header));
}

// https://w3c.github.io/webappsec-fetch-metadata/#abstract-opdef-set-user
void set_sec_fetch_user_header(Infrastructure::Request& request)
{
    // 1. Assert: r’s url is a potentially trustworthy URL.
    VERIFY(SecureContexts::is_url_potentially_trustworthy(request.url()) == SecureContexts::Trustworthiness::PotentiallyTrustworthy);

    // 2. If r is not a navigation request, or if r’s user-activation is false, return.
    if (!request.is_navigation_request() || !request.user_activation())
        return;

    // 3. Let header be a Structured Header whose value is a token.
    // FIXME: This is handled below, as Serenity doesn't have APIs for RFC 8941.

    // 4. Set header’s value to true.
    // NOTE: See https://datatracker.ietf.org/doc/html/rfc8941#name-booleans for boolean format in RFC 8941.
    auto header_value = MUST(ByteBuffer::copy("?1"sv.bytes()));

    // 5. Set a structured field value `Sec-Fetch-User`/header in r’s header list.
    auto header = Infrastructure::Header {
        .name = MUST(ByteBuffer::copy("Sec-Fetch-User"sv.bytes())),
        .value = move(header_value),
    };
    request.header_list()->append(move(header));
}

// https://w3c.github.io/webappsec-fetch-metadata/#abstract-opdef-append-the-fetch-metadata-headers-for-a-request
void append_fetch_metadata_headers_for_request(Infrastructure::Request& request)
{
    // 1. If r’s url is not an potentially trustworthy URL, return.
    if (SecureContexts::is_url_potentially_trustworthy(request.url()) != SecureContexts::Trustworthiness::PotentiallyTrustworthy)
        return;

    // 2. Set the Sec-Fetch-Dest header for r.
    set_sec_fetch_dest_header(request);

    // 3. Set the Sec-Fetch-Mode header for r.
    set_sec_fetch_mode_header(request);

    // 4. Set the Sec-Fetch-Site header for r.
    set_sec_fetch_site_header(request);

    // 5. Set the Sec-Fetch-User header for r.
    set_sec_fetch_user_header(request);
}

}
