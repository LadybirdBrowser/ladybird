/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/Method.h>
#include <LibJS/Runtime/Completion.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/FetchEventSink.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Fetching/NetworkBodyPump.h>
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Fetching/RefCountedFlag.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/PreloadEntry.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/CompiledScriptCache.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SupportedImageTypes.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/Clocks.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::Fetch::Fetching {

// https://fetch.spec.whatwg.org/#document-accept-header-value
// The document `Accept` header value is `text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8`.
constexpr auto document_accept_header_value = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"sv;

// AD-HOC: Reads one chunk of a response body's readable byte stream and forwards it into another readable byte stream,
//         running processResponseEndOfBody once the stream has no more chunks. See finish_fetch_response_handover().
class ResponseBodyForwardingReadRequest final : public Streams::ReadRequest {
    GC_CELL(ResponseBodyForwardingReadRequest, Streams::ReadRequest);
    GC_DECLARE_ALLOCATOR(ResponseBodyForwardingReadRequest);

public:
    ResponseBodyForwardingReadRequest(GC::Ref<Streams::ReadableStream> destination, GC::Ref<WebIDL::Promise> pull_promise, GC::Ref<GC::Function<void()>> process_response_end_of_body)
        : m_destination(destination)
        , m_pull_promise(pull_promise)
        , m_process_response_end_of_body(process_response_end_of_body)
    {
    }

    virtual void on_chunk(JS::Value chunk) override
    {
        if (m_destination->enqueue(chunk).is_error())
            m_destination->error(JS::TypeError::create(WebIDL::promise_realm(*m_pull_promise), "Response body chunk could not be enqueued"sv));
        WebIDL::resolve_promise(*m_pull_promise);
    }

    virtual void on_close() override
    {
        // NB: Canceling the destination cancels the source, which closes this read request. As with a TransformStream
        //     whose readable side is canceled, processResponseEndOfBody does not run then.
        if (m_destination->is_readable()) {
            // NB: Like the flush algorithm of a TransformStream, run processResponseEndOfBody before the stream closes,
            //     so that it runs before anything that waits for the stream to close.
            m_process_response_end_of_body->function()();
            m_destination->close();
        }
        WebIDL::resolve_promise(*m_pull_promise);
    }

    virtual void on_error(JS::Value error) override
    {
        m_destination->error(error);
        WebIDL::resolve_promise(*m_pull_promise);
    }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_destination);
        visitor.visit(m_pull_promise);
        visitor.visit(m_process_response_end_of_body);
    }

    GC::Ref<Streams::ReadableStream> m_destination;
    GC::Ref<WebIDL::Promise> m_pull_promise;
    GC::Ref<GC::Function<void()>> m_process_response_end_of_body;
};

GC_DEFINE_ALLOCATOR(ResponseBodyForwardingReadRequest);

// The view of the body of response, which takes the channel the body's bytes arrive through.
static GC::Ptr<Infrastructure::Body> create_body_view(JS::Realm& realm, Infrastructure::Response& response)
{
    auto body = response.body();
    if (!body.has_value())
        return {};

    if (auto const* bytes = body->bytes().get_pointer<Core::ImmutableBytes>())
        return Infrastructure::Body::create_over_bytes(realm, *bytes);

    auto channel = body->bytes().get<NonnullRefPtr<Engine::FetchByteChannel>>();
    auto length = body->length();
    response.set_body(Infrastructure::ResponseBody { Infrastructure::ResponseBody::ReadThroughStream {}, length });
    return create_body_from_channel(realm, nullptr, move(channel));
}

// https://fetch.spec.whatwg.org/#concept-fetch
// NB: The WebSocket opening handshake does not go through fetch yet, so it runs this step on its own.
void append_accept_language_header_if_missing(Infrastructure::Request& request, LoaderConfig const& loader_config)
{
    if (!request.header_list()->contains("Accept-Language"sv)) {
        StringBuilder accept_language;
        accept_language.join(","sv, loader_config.preferred_languages);

        auto header = HTTP::Header::isomorphic_encode("Accept-Language"sv, accept_language.string_view());
        request.header_list()->append(move(header));
    }
}

// https://fetch.spec.whatwg.org/#concept-fetch
GC::Ref<Infrastructure::FetchController> fetch(JS::Realm& realm, Infrastructure::Request& request, Infrastructure::FetchAlgorithms const& algorithms, UseParallelQueue use_parallel_queue, CreateResponseBodyTransferLease create_response_body_transfer_lease)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Running 'fetch' with: request @ {}", &request);

    // 1. Assert: request’s mode is "navigate" or processEarlyHintsResponse is null.
    VERIFY(request.mode() == Infrastructure::Request::Mode::Navigate || !algorithms.process_early_hints_response());

    // 2. Let taskDestination be null.
    Infrastructure::TaskDestination task_destination;

    // 3. Let crossOriginIsolatedCapability be false.
    auto cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No;

    auto client = resolve_fetch_client(request);

    // 4. Populate request from client given request.
    populate_request_from_client(request, client);

    // 5. If request’s client is non-null, then:
    if (client.has_value()) {
        // 1. Set taskDestination to request’s client’s global object.
        // AD-HOC: A client another process hosts has its global object there, so the fetch's tasks run in the realm
        //         that started it.
        task_destination = client->visit(
            [](GC::Ref<HTML::EnvironmentSettingsObject> const& settings_object) { return GC::Ref { settings_object->global_object() }; },
            [&](NonnullRefPtr<HTML::RemoteEnvironmentSettings const> const&) { return GC::Ref { realm.global_object() }; });

        // 2. Set crossOriginIsolatedCapability to request’s client’s cross-origin isolated capability.
        cross_origin_isolated_capability = request.client_snapshot()->cross_origin_isolated_capability;
    }

    // 6. If useParallelQueue is true, then set taskDestination to the result of starting a new parallel queue.
    if (use_parallel_queue == UseParallelQueue::Yes)
        task_destination = HTML::ParallelQueue::create();

    // 7. Let timingInfo be a new fetch timing info whose start time and post-redirect start time are the coarsened
    //    shared current time given crossOriginIsolatedCapability, and render-blocking is set to request’s
    //    render-blocking.
    auto timing_info = Infrastructure::FetchTimingInfo::create();
    auto now = HighResolutionTime::coarsened_shared_current_time(cross_origin_isolated_capability);
    timing_info->set_start_time(now);
    timing_info->set_post_redirect_start_time(now);
    timing_info->set_render_blocking(request.render_blocking());

    // 8. Let fetchParams be a new fetch params whose request is request, timing info is timingInfo, process request
    //    body chunk length is processRequestBodyChunkLength, process request end-of-body is processRequestEndOfBody,
    //    process early hints response is processEarlyHintsResponse, process response is processResponse, process
    //    response consume body is processResponseConsumeBody, process response end-of-body is processResponseEndOfBody,
    //    task destination is taskDestination, and cross-origin isolated capability is crossOriginIsolatedCapability.
    auto& agent = HTML::relevant_agent(realm.global_object());
    VERIFY(agent.fetch_executor);
    auto controller = Infrastructure::FetchController::create(agent.fetch_controller_registry);
    auto fetch_params = controller->create_fetch_params(request, algorithms, timing_info, current_loader_config());
    controller->set_realm(realm);
    controller->set_task_destination(task_destination);
    controller->set_keepalive(request.keepalive());
    // AD-HOC: A fetch's network requests are loaded and reported for the page of the realm that starts it.
    auto& page = Bindings::principal_host_defined_page(realm);
    controller->set_page(page);
    // NB: Workers have a Page shim without a navigable or a browser page ID. Their requests are attributed through the
    //     worker's browser-level owners.
    if (page.has_top_level_traversable())
        fetch_params->set_originating_page_id(page.client().id());
    fetch_params->set_cross_origin_isolated_capability(cross_origin_isolated_capability);
    fetch_params->set_has_response_body_transfer_lease(create_response_body_transfer_lease == CreateResponseBodyTransferLease::Yes);

    // 9. If request’s body is a byte sequence, then set request’s body to request’s body as a body.
    if (auto* buffer = request.body().get_pointer<ByteBuffer>())
        request.set_body(Infrastructure::FetchBody::from_bytes(Core::ImmutableBytes::adopt(move(*buffer))));

    // AD-HOC: The blob URL entry of request’s URL is resolved here, when the fetch starts, so that scheme fetch needs no
    //         page. It is the entry of request’s current URL there, since no redirect leads to a blob URL.
    if (request.url().scheme() == "blob"sv) {
        auto& page = Bindings::principal_host_defined_page(realm);
        auto blob_url_entry = FileAPI::blob_url_entry_in_the_user_agent_store(page, request.url());
        fetch_params->set_blob_url_entry(move(blob_url_entry));
    }

    auto local_client = HTML::settings_object_of_fetch_client(client);
    auto* client_window = local_client ? HTML::window_from_global_object(local_client->global_object()) : nullptr;

    // 10. If all of the following conditions are true:
    if (
        // - request’s URL’s scheme is an HTTP(S) scheme
        Infrastructure::is_http_or_https_scheme(request.url().scheme())
        // - request’s mode is "same-origin", "cors", or "no-cors"
        && (request.mode() == Infrastructure::Request::Mode::SameOrigin || request.mode() == Infrastructure::Request::Mode::CORS || request.mode() == Infrastructure::Request::Mode::NoCORS)
        // - request’s client is not null, and request’s client’s global object is a Window object
        // AD-HOC: A request whose Window client another process hosts consumes no preloaded resource: the client's
        //         document, which holds its preloads, is in that process.
        && client_window
        // - request’s method is `GET`
        && request.method().equals_ignoring_ascii_case("GET"sv)
        // - request’s unsafe-request flag is not set or request’s header list is empty
        && (!request.unsafe_request() || request.header_list()->is_empty())) {
        // 1. Assert: request’s origin is same origin with request’s client’s origin.
        VERIFY(request.origin().has<URL::Origin>() && request.origin().get<URL::Origin>().is_same_origin(request.client_snapshot()->origin));

        // 2. Let onPreloadedResponseAvailable be an algorithm that runs the following step given a response
        //    response: set fetchParams’s preloaded response candidate to response.
        auto on_preloaded_response_available = GC::create_function(GC::Heap::the(), GC::captures(fetch_params, controller), [](auto& fetch_params, auto& controller, RefPtr<Infrastructure::Response> response) {
            // AD-HOC: A fetch whose preload was cancelled reports its own timing, as one that found no preload does.
            if (!response)
                controller->set_preload_reports_timing(false);

            // NB: Once the fetch has started, its engine runs main fetch, which waits for the response there.
            if (auto fetch_id = controller->started_fetch_id(); fetch_id.has_value()) {
                controller->started_fetch_executor().post(Engine::Command::PreloadResponseResolved { *fetch_id, move(response) });
                return;
            }
            if (response)
                fetch_params->set_preloaded_response_candidate(response.release_nonnull());
            else
                fetch_params->set_preloaded_response_candidate(Empty {});
        });

        // 3. Let foundPreloadedResource be the result of invoking consume a preloaded resource for request’s
        //    window, given request’s URL, request’s destination, request’s mode, request’s credentials mode,
        //    request’s integrity metadata, and onPreloadedResponseAvailable.
        bool preload_reports_timing = false;
        auto found_preloaded_resource = HTML::consume_a_preloaded_resource(
            *client_window,
            request.url(),
            request.destination(),
            request.mode(),
            request.credentials_mode(),
            request.integrity_metadata(),
            on_preloaded_response_available,
            fetch_params->control_block().body_intent(),
            preload_reports_timing);
        controller->set_preload_reports_timing(preload_reports_timing);

        // 4. If foundPreloadedResource is true and fetchParams’s preloaded response candidate is null, then set
        //    fetchParams’s preloaded response candidate to "pending".
        if (found_preloaded_resource && fetch_params->preloaded_response_candidate().has<Empty>())
            fetch_params->set_preloaded_response_candidate(Infrastructure::FetchParams::PreloadedResponseCandidatePendingTag {});
    }

    // 11. If request’s header list does not contain `Accept`, then:
    if (!request.header_list()->contains("Accept"sv)) {
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
                // AD-HOC: The spec default omits AVIF, JPEG XL and WebP, so CDNs that perform format negotiation do not
                //         send them, and potentially fall back to non alpha-preserving formats.
                //         Spec issue: https://github.com/whatwg/fetch/issues/1740
                value = "image/avif,image/jxl,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5"sv;
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
        auto header = HTTP::Header::isomorphic_encode("Accept"sv, value);
        request.header_list()->append(move(header));
    }

    // 12. If request’s header list does not contain `Accept-Language`, then user agents should append
    //     (`Accept-Language, an appropriate header value) to request’s header list.
    append_accept_language_header_if_missing(request, fetch_params->loader_config());

    // 13. If request’s internal priority is null, then use request’s priority, initiator, destination, and
    //     render-blocking in an implementation-defined manner to set request’s internal priority to an
    //     implementation-defined object.
    // NOTE: The user-agent-defined object could encompass stream weight and dependency for HTTP/2, and equivalent
    //       information used to prioritize dispatch and processing of HTTP/1 fetches.

    // 14. If request is a subresource request, then:
    if (request.is_subresource_request()) {
        // 1. Let record be a new fetch record whose request is request and controller is fetchParams’s controller.
        auto record = Infrastructure::FetchRecord::create(request, controller);

        // 2. Append record to request’s client’s fetch group’s fetch records.
        // AD-HOC: A client another process hosts has its fetch group there.
        if (local_client)
            local_client->fetch_group().append(record);

        // AD-HOC: A keepalive request's body length counts toward its fetch group's keepalive quota from here until
        //         its done flag is set, its body is null or the request is destroyed, since its fetch record can be
        //         collected before then.
        if (request.keepalive()) {
            auto body_length = request.body().visit(
                [](Empty) -> u64 { return 0; },
                [](ByteBuffer const& buffer) -> u64 { return buffer.size(); },
                [](Infrastructure::FetchBody const& body) -> u64 { return body.length().value_or(0); });
            request.set_keepalive_quota_reservation(request.client_snapshot()->keepalive_quota_accountant->reserve(body_length));
        }
    }

    if (Core::StatsLog::the() && client_window && request.is_subresource_request())
        begin_rendering_pending_operation(controller.ptr(), client_window->associated_document(), RenderingPendingReason::ResourceFetchesInFlight);

    // 15. Run main fetch given fetchParams.
    main_fetch(fetch_params, ContentSecurityPolicy::violation_reporter_for_request(realm, request), agent.fetch_executor->known_hsts_host_provider());

    // 16. Return fetchParams’s controller.
    return controller;
}

// https://fetch.spec.whatwg.org/#populate-request-from-client
void populate_request_from_client(Infrastructure::Request& request, Optional<HTML::FetchClient> const& client)
{
    auto local_client = HTML::settings_object_of_fetch_client(client);

    // 1. If request’s traversable for user prompts is "client":
    auto const* traversable_for_user_prompts = request.traversable_for_user_prompts().get_pointer<Infrastructure::Request::TraversableForUserPrompts>();
    if (traversable_for_user_prompts && *traversable_for_user_prompts == Infrastructure::Request::TraversableForUserPrompts::Client) {
        // 1. Set request’s traversable for user prompts to "no-traversable".
        request.set_traversable_for_user_prompts(Infrastructure::Request::TraversableForUserPrompts::NoTraversable);

        // 2. If request’s client is non-null:
        // AD-HOC: The settings of a client another process hosts do not name its global object's navigable, so its
        //         requests have no traversable for user prompts.
        if (local_client) {
            // 1. Let global be request’s client’s global object.
            auto& global = local_client->global_object();

            // 2. If global is a Window object and global’s navigable is not null, then set request’s traversable for
            //    user prompts to global’s navigable’s traversable navigable.
            if (auto const* window = HTML::window_from_global_object(global)) {
                if (window->navigable())
                    request.set_traversable_for_user_prompts(window->navigable()->traversable_navigable()->id());
            }
        }
    }

    // 2. If request’s origin is "client":
    auto const* origin = request.origin().get_pointer<Infrastructure::Request::Origin>();
    if (origin && *origin == Infrastructure::Request::Origin::Client) {
        // 1. Assert: request’s client is non-null.
        VERIFY(client.has_value());

        // 2. Set request’s origin to request’s client’s origin.
        request.set_origin(HTML::origin_of_fetch_client(*client));
    }

    // 3. If request’s policy container is "client":
    auto const* policy_container = request.policy_container().get_pointer<Infrastructure::Request::PolicyContainer>();
    if (policy_container && *policy_container == Infrastructure::Request::PolicyContainer::Client) {
        // 1. If request’s client is non-null, then set request’s policy container to a clone of request’s client’s
        //    policy container.
        if (client.has_value()) {
            request.set_policy_container(client->visit(
                [](GC::Ref<HTML::EnvironmentSettingsObject> const& settings_object) { return settings_object->policy_container()->clone(); },
                [](NonnullRefPtr<HTML::RemoteEnvironmentSettings const> const& settings) {
                    return HTML::create_a_policy_container_from_serialized_policy_container(settings->settings.policy_container);
                }));
        }
        // 2. Otherwise, set request’s policy container to a new policy container.
        else {
            request.set_policy_container(HTML::PolicyContainer::create());
        }
    }

    // AD-HOC: Take a snapshot of the client for main fetch and the steps under it, which reach the client itself only
    //         through its global object, to queue tasks and to report violations and timing.
    if (client.has_value())
        request.set_client_snapshot(client->visit([](auto const& client) { return snapshot_client_context(*client); }));
}

GC::Ref<Infrastructure::FetchController> fetch_controller_of(Infrastructure::FetchParams const& fetch_params)
{
    auto const& registry = fetch_params.controller_registry();
    VERIFY(registry);
    auto controller = registry->find(fetch_params.controller_token());
    VERIFY(controller);
    return *controller;
}

// https://fetch.spec.whatwg.org/#concept-main-fetch
// Main fetch given recursive false continues at step 11.
static void continue_main_fetch_in_parallel(Infrastructure::FetchParams const& fetch_params, RefPtr<Infrastructure::Response> response)
{
    auto request = fetch_params.request();

    // 11. If recursive is false, then run the remaining steps in parallel.
    auto controller = fetch_controller_of(fetch_params);

    // AD-HOC: A manual redirect's next hop runs under its fetch's id once the engine is told of the follow. It does not
    //         run for a redirect answered before, or for a fetch no longer ongoing, whose hop could only stop or fail.
    if (controller->started_fetch_id().has_value()) {
        if (controller->has_unanswered_redirect()) {
            controller->set_observed_request(observed_request(request, fetch_params.timing_info()));
            Engine::MainFetchInput next_hop { NonnullRefPtr { fetch_params }, response };
            controller->resolve_redirect(Engine::Command::ResolveRedirect::Follow { move(next_hop) });
        }
        return;
    }

    VERIFY(fetch_params.controller_registry());
    auto& controller_registry = *fetch_params.controller_registry();
    auto fetch_id = Infrastructure::FetchControllerRegistry::next_fetch_id();
    controller_registry.root_fetch(fetch_id, controller);
    auto& fetch_executor = *HTML::surrounding_agent().fetch_executor;
    controller->set_started_fetch(fetch_id, fetch_executor);
    controller->set_observed_request(observed_request(request, fetch_params.timing_info()));
    auto start_fetch_input = fetch_params.start_fetch_input(fetch_id, response, make<FetchEventSink>(*controller), PageNetworkLoader::create(controller->page()));
    fetch_executor.post(Engine::Command::StartFetch { move(start_fetch_input) });

    // NB: A fetch stopped, aborted or terminated before it started is stopped once it starts.
    if (!fetch_params.control_block().is_ongoing())
        controller->stop_request();
}

// https://fetch.spec.whatwg.org/#concept-main-fetch
// NB: This is main fetch given recursive false, which the agent runs for a fetch and a manual redirect's next hop.
void main_fetch(Infrastructure::FetchParams const& fetch_params, ContentSecurityPolicy::ViolationReporter const& report_violation, Engine::KnownHSTSHostProvider& known_hsts_host_provider)
{
    auto steps = prepare_main_fetch(fetch_params, report_violation, known_hsts_host_provider);
    if (!steps.host_to_look_up.has_value()) {
        continue_main_fetch_in_parallel(fetch_params, move(steps.response));
        return;
    }

    auto controller = GC::make_root(fetch_controller_of(fetch_params));
    known_hsts_host_provider.look_up(fetch_params, *steps.host_to_look_up, [fetch_params = NonnullRefPtr { fetch_params }, response = move(steps.response), controller = move(controller)](bool is_known_hsts_host) mutable {
        if (is_known_hsts_host)
            upgrade_request_to_https(fetch_params->request());
        continue_main_fetch_in_parallel(*fetch_params, move(response));
    });
}

// https://mimesniff.spec.whatwg.org/#minimize-a-supported-mime-type
// NB: Which MIME types the user agent supports is left to the user agent. These are HTML, CSS, plain text, WebVTT, the
//     image types that are decoded and the audio and video containers that are demuxed.
static bool is_supported_by_the_user_agent(MimeSniff::MimeType const& mime_type)
{
    if (mime_type.is_html() || mime_type.essence().is_one_of("text/css"sv, "text/plain"sv, "text/vtt"sv))
        return true;
    if (mime_type.is_image())
        return HTML::is_supported_image_type(Utf16String::from_utf8(mime_type.essence()));
    if (mime_type.is_audio_or_video()) {
        auto container = Media::container_mime_type_from_mime_type(mime_type.type(), mime_type.subtype());
        return container.has_value() && Media::is_supported_file_container(*container);
    }
    return false;
}

// https://fetch.spec.whatwg.org/#fetch-finale
void finish_fetch_response_handover(GC::Ref<Infrastructure::FetchController> controller, Engine::Event::ResponseDelivery delivery)
{
    auto& realm = controller->realm();
    auto request = controller->request();
    auto const& response = *delivery.response;
    controller->set_redirect_continuation_id(delivery.manual_redirect.has_value() ? delivery.manual_redirect->continuation_id : Optional<Engine::ContinuationId> {});

    // https://fetch.spec.whatwg.org/#concept-http-fetch
    // NB: This is HTTP fetch's step for a navigation's manual redirect, which sets fetchParams's controller's next
    //     manual redirect steps to run HTTP-redirect fetch given fetchParams and response.
    if (auto const& manual_redirect = delivery.manual_redirect; manual_redirect.has_value()) {
        controller->set_next_manual_redirect_steps(GC::create_function(controller->heap(), GC::captures(manual_redirect->fetch_params, manual_redirect->response), [](NonnullRefPtr<Infrastructure::FetchParams const>& fetch_params, NonnullRefPtr<Infrastructure::Response>& response) {
            // https://fetch.spec.whatwg.org/#concept-http-redirect-fetch
            auto steps = prepare_redirect_hop(*fetch_params, *response);
            auto controller = fetch_controller_of(*fetch_params);

            // AD-HOC: Abandon the redirect when HTTP-redirect fetch returns early, as nothing else answers it.
            if (steps.has<NonnullRefPtr<Infrastructure::Response>>()) {
                controller->resolve_redirect(Engine::Command::ResolveRedirect::Abandon {});
                return;
            }
            VERIFY(steps.get<Recursive>() == Recursive::No);

            // 22. Return the result of running main fetch given fetchParams and recursive.
            auto& realm = controller->realm();
            auto& known_hsts_host_provider = controller->started_fetch_executor().known_hsts_host_provider();
            main_fetch(*fetch_params, ContentSecurityPolicy::violation_reporter_for_request(realm, fetch_params->request()), known_hsts_host_provider);
        }));
    }

    // NB: The body view takes the channel that the internal response's body arrives through.
    auto body_view = delivery.body.visit(
        [](Engine::NullBody) -> GC::Ptr<Infrastructure::Body> { return {}; },
        [&](Engine::Delivered<Engine::NonNullLocalBodyHandle> const& delivered) {
            return delivered.handle().visit(
                [&](Core::ImmutableBytes const& bytes) -> GC::Ptr<Infrastructure::Body> { return Infrastructure::Body::create_over_bytes(realm, bytes); },
                [&](NonnullRefPtr<Engine::FetchByteChannel> const& channel) -> GC::Ptr<Infrastructure::Body> { return create_body_from_channel(realm, controller, channel); });
        });
    auto response_view = Infrastructure::ResponseView::create(delivery.response, body_view);

    // AD-HOC: timingInfo belongs to the fetch's engine, so the steps below use the agent's own copy of it, made of the
    //         fields settled so far, the network phases settled last and the end time last set for the fetch. Those
    //         settled or set later reach the latest handover's copy.
    auto agent_timing_info = controller->control_block().hand_over_timing(delivery.timing_seed);

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
    // NOTE: The create and initialize a Document object algorithm extracts full timing info for navigation requests
    //       whose destination can also be "embed", "frame", "iframe", or "object".

    // 3. If fetchParams's request's destination is "document", then set fetchParams's controller's full timing info
    //    to fetchParams's timing info.
    if (request->is_navigation_request())
        controller->control_block().set_full_timing_info(agent_timing_info);

    // AD-HOC: We extract step 2 of processResponseEndOfBody into a separate lambda, given unsafeEndTime, so that the
    //         error path and network completion can also run it. The fetch spec only runs processResponseEndOfBody on
    //         successful body read (via the transform stream's flush algorithm). However, processResponseConsumeBody
    //         is called for both success and failure, and specs like HTML's preload algorithm expect to be able to
    //         call reportTiming from within processResponseConsumeBody. So we ensure report_timing_steps is set on
    //         error too, which allows reportTiming to work without asserting, and still produces useful timing data
    //         for failed fetches.
    auto timing_reported = RefCountedFlag::create(false);
    auto setup_report_timing_steps = GC::create_function(GC::Heap::the(), GC::captures(response_view->record(), request, controller, agent_timing_info, timing_reported), [](auto& response, auto& request, auto& controller, auto& timing_info, auto& timing_reported, HighResolutionTime::DOMHighResTimeStamp unsafe_end_time) {
        // 2. Set fetchParams’s controller’s report timing steps to the following steps given a global object global:
        NonnullRefPtr control_block = controller->control_block();
        controller->set_report_timing_steps(GC::create_function(GC::Heap::the(), GC::captures(response, request, control_block, timing_info, unsafe_end_time, timing_reported), [](auto& response, auto& request, auto& control_block, auto& timing_info, auto& unsafe_end_time, auto& timing_reported, JS::Object& global) {
            // NB: Network completion and consuming the response body can both request a timing report.
            if (timing_reported->value())
                return;
            timing_reported->set_value(true);

            // 1. If fetchParams’s request’s URL’s scheme is not an HTTP(S) scheme, then return.
            if (!Infrastructure::is_http_or_https_scheme(request->url().scheme()))
                return;

            // 2. Set timingInfo’s end time to the relative high resolution time given unsafeEndTime and global.
            // FIXME: Spec issue: Using relative time here is incorrect, as end time is converted to relative time by
            //        Resource Timing, causing it to take a relative time of an already relative time, effectively make
            //        it always a negative value approximately the value of the time origin.
            //        https://github.com/whatwg/fetch/issues/1812
            timing_info->set_end_time(unsafe_end_time);
            // AD-HOC: The agent keeps this as the end time last set for the fetch, which later handovers and WebDriver
            //         BiDi's events take.
            control_block->set_end_time(unsafe_end_time);

            // 3. Let cacheState be response’s cache state.
            auto cache_state = response->cache_state();

            // 4. Let bodyInfo be response’s body info.
            auto body_info = response->body_info();

            // 5. If response’s timing allow passed flag is not set, then set timingInfo to the result of creating an
            //    opaque timing info for timingInfo, set bodyInfo to a new response body info, and set cacheState to
            //    the empty string.
            // NOTE: This covers the case of response being a network error.
            if (!response->timing_allow_passed()) {
                timing_info = Infrastructure::create_opaque_timing_info(timing_info);
                body_info = Infrastructure::Response::BodyInfo {};
                cache_state = {};
            }

            // 6. Let responseStatus be 0.
            auto response_status = 0;

            // 7. If fetchParams’s request’s mode is not "navigate" or response’s redirect taint is "same-origin":
            if (request->mode() != Infrastructure::Request::Mode::Navigate || response->redirect_taint() == Infrastructure::RedirectTaint::SameOrigin) {
                // 1. Set responseStatus to response’s status.
                response_status = response->status();

                // 2. Let mimeType be the result of extracting a MIME type from response’s header list.
                auto mime_type = Infrastructure::extract_mime_type(response->header_list());

                // 3. If mimeType is non-null, then set bodyInfo’s content type to the result of minimizing a supported MIME type given mimeType.
                if (mime_type.has_value())
                    body_info.content_type = Utf16String::from_utf8(MimeSniff::minimise_a_supported_mime_type(mime_type.value(), is_supported_by_the_user_agent(mime_type.value())));
            }

            // 8. If fetchParams’s request’s initiator type is not null, then mark resource timing given timingInfo,
            //    request’s URL, request’s initiator type, global, cacheState, bodyInfo, and responseStatus.
            if (request->initiator_type().has_value()) {
                auto initiator_type = Infrastructure::initiator_type_to_string(request->initiator_type().value());
                ResourceTiming::mark_resource_timing(timing_info, utf16_string_from_url_ascii(request->url().serialize()), initiator_type, global, cache_state, body_info, response_status);
            }
        }));
    });

    // NB: Share step 3 of processResponseEndOfBodyTask with the network-completion timing path.
    auto report_timing_if_appropriate = GC::create_function(GC::Heap::the(), GC::captures(request, controller), [](NonnullRefPtr<Infrastructure::Request>& request, GC::Ref<Infrastructure::FetchController>& controller) {
        // 3. If fetchParams’s request’s initiator type is non-null and fetchParams’s request’s client’s global
        //    object is fetchParams’s task destination, then run fetchParams’s controller’s report timing steps
        //    given fetchParams’s request’s client’s global object.
        // AD-HOC: Not when this fetch consumed a preload that reports its own timing. Other engines report a single
        //         entry for a preloaded resource and its consumer.
        // AD-HOC: The global object of a client another process hosts is in that process, so the fetch reports its
        //         timing to the global object of the realm that started it.
        auto const* task_destination_global_object = controller->task_destination().get_pointer<GC::Ref<JS::Object>>();
        if (task_destination_global_object != nullptr && request->initiator_type().has_value() && !controller->preload_reports_timing()) {
            auto client = resolve_fetch_client(request);
            auto client_global_is_task_destination = client.has_value()
                && client->visit(
                    [&](GC::Ref<HTML::EnvironmentSettingsObject> const& settings_object) { return &settings_object->global_object() == task_destination_global_object->ptr(); },
                    [](NonnullRefPtr<HTML::RemoteEnvironmentSettings const> const&) { return true; });
            if (client_global_is_task_destination)
                controller->report_timing(**task_destination_global_object);
        }
    });

    // 4. Let processResponseEndOfBody be the following steps:
    auto process_response_end_of_body = GC::create_function(GC::Heap::the(), GC::captures(request, controller, response_view, agent_timing_info, setup_report_timing_steps, report_timing_if_appropriate), [](auto& request, auto& controller, auto& response_view, auto& agent_timing_info, auto& setup_report_timing_steps, auto& report_timing_if_appropriate) {
        // 1. Let unsafeEndTime be the unsafe shared current time.
        auto unsafe_end_time = HighResolutionTime::unsafe_shared_current_time();

        // 2. (See setup_report_timing_steps above)
        setup_report_timing_steps->function()(unsafe_end_time);
        if (!response_view->is_network_error())
            report_response_completed_to_observers(controller, *response_view->record()->unsafe_response(), *agent_timing_info);

        // 3. Let processResponseEndOfBodyTask be the following steps:
        auto process_response_end_of_body_task = GC::create_function(GC::Heap::the(), GC::captures(request, controller, response_view, report_timing_if_appropriate), [](auto& request, auto& controller, auto& response_view, auto& report_timing_if_appropriate) {
            // 1. Set fetchParams’s request’s done flag.
            request->set_done(true);
            end_rendering_pending_operation(controller.ptr());

            // 2. If fetchParams’s process response end-of-body is non-null, then run fetchParams’s process response
            //    end-of-body given response.
            auto algorithms = controller->algorithms();
            if (algorithms->process_response_end_of_body())
                algorithms->process_response_end_of_body()->function()(response_view);

            report_timing_if_appropriate->function()();
        });

        // 4. Queue a fetch task to run processResponseEndOfBodyTask with fetchParams’s task destination.
        controller->queue_fetch_task(move(process_response_end_of_body_task));
    });

    // 5. If fetchParams’s process response is non-null, then queue a fetch task to run fetchParams’s process response
    //    given response, with fetchParams’s task destination.
    if (controller->algorithms()->process_response()) {
        controller->queue_fetch_task(GC::create_function(GC::Heap::the(), GC::captures(controller, response_view), [](auto& controller, auto& response_view) {
            controller->algorithms()->process_response()->function()(response_view);
        }));
    }

    // 6. Let internalResponse be response, if response is a network error; otherwise response’s internal response.
    auto internal_response = response.is_network_error() ? delivery.response : response.unsafe_response();

    // AD-HOC: Report a body's timing once the network has delivered it. Fetch reports it from the flush of the
    //         transform the body is piped through, which backpressure on an unread body can hold back.
    if (!delivery.body.has<Engine::NullBody>()) {
        GC::Ptr<GC::Function<void(HighResolutionTime::DOMHighResTimeStamp)>> network_completion_steps;
        auto observes_fetches = HTML::surrounding_agent().network_activity_observers->observes_fetches();
        if (request->initiator_type().has_value() || (observes_fetches && !request->is_navigation_request())) {
            network_completion_steps = GC::create_function(GC::Heap::the(), GC::captures(controller, response_view, agent_timing_info, setup_report_timing_steps, report_timing_if_appropriate), [](auto& controller, auto& response_view, auto& agent_timing_info, auto& setup_report_timing_steps, auto& report_timing_if_appropriate, HighResolutionTime::DOMHighResTimeStamp unsafe_end_time) {
                setup_report_timing_steps->function()(unsafe_end_time);
                report_timing_if_appropriate->function()();
                report_response_completed_to_observers(controller, *response_view->record()->unsafe_response(), *agent_timing_info);
            });
        }
        // NB: Only a channel still being written at the handover has a delivery id, which its EndOfBody carries.
        auto const& handle = delivery.body.get<Engine::Delivered<Engine::NonNullLocalBodyHandle>>().handle();
        auto const* channel = handle.get_pointer<NonnullRefPtr<Engine::FetchByteChannel>>();
        auto delivery_id = channel ? (*channel)->delivery_id() : Optional<Engine::DeliveryId> {};
        if (delivery_id.has_value())
            controller->when_body_ends(*delivery_id, network_completion_steps);
        else if (network_completion_steps)
            network_completion_steps->function()(HighResolutionTime::unsafe_shared_current_time());
    }

    if (Core::StatsLog::the() && body_view)
        track_rendering_pending_stream(controller.ptr(), *body_view->stream());

    auto algorithms = controller->algorithms();

    // NB: Nobody reads the body of a fetch stopped before its handover.
    auto body_intent = controller->control_block().is_stopped()
        ? Engine::BodyIntent::DrainAndDiscard
        : controller->control_block().body_intent();

    // AD-HOC: A fetch whose body intent is SyncAccumulate is a sync XHR send()
    //         (https://xhr.spec.whatwg.org/#the-send()-method, step 12) — or a preload fetch that "consume a preloaded
    //         resource" re-targeted for one. While that send() is blocked, the HTML event loop is
    //         paused (https://html.spec.whatwg.org/multipage/#pause): It runs no tasks and performs no microtask
    //         checkpoints. But the identity-TransformStream pipe in step 7 and Body::fully_read() in step 8.4 progress
    //         only thru promise-reaction microtasks — so they'd never complete. Nor could they when send() was invoked
    //         from within a microtask: Performing a microtask checkpoint is non-reentrant. The spec runs all of this in
    //         parallel, off the event loop. So, for such a fetch, read the internal response's stream directly (chunk
    //         delivery and stream close fulfill pending read requests synchronously), and once the read completes, run
    //         processResponseEndOfBody — which the pipe's flush algorithm would otherwise have — and then processBody,
    //         in the order the parallel queue would have run them.
    bool read_body_in_parallel = body_intent == Engine::BodyIntent::SyncAccumulate;

    // AD-HOC: That direct read runs processResponseEndOfBody and processBody itself. processResponse (step 4 above)
    //         and processResponseEndOfBody's task (step 3) are queued either way — onto a parallel queue backed by the
    //         event loop's task queue (ParallelQueue::enqueue) — so they'd stay frozen til send() unpauses the loop,
    //         and run after the body they precede was consumed. No caller sets either; assert it, so that adding one
    //         fails here instead of silently reordering the algorithms.
    if (read_body_in_parallel) {
        VERIFY(algorithms->process_response_consume_body());
        VERIFY(!algorithms->process_response());
        VERIFY(!algorithms->process_response_end_of_body());
    }

    // 7. If internalResponse’s body is null, then run processResponseEndOfBody.
    if (!internal_response->body().has_value()) {
        process_response_end_of_body->function()();
    }
    // 8. Otherwise:
    else {
        switch (body_intent) {
        case Engine::BodyIntent::DeliverStreamToConsumerAgent:
        case Engine::BodyIntent::ConsumeForCallback: {
            HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // AD-HOC: Piping a readable byte stream through a TransformStream results in a stream without byte reading
            //         support, which can no longer be read with a BYOB reader. Response bodies remain readable byte streams
            //         in other engines, so forward the chunks of such a stream into a new readable byte stream instead, and
            //         run processResponseEndOfBody once there are no more chunks, as the flush algorithm below would.
            if (auto stream = body_view->stream(); stream->is_readable_byte_stream() && !stream->is_locked()) {
                auto reader = MUST(stream->get_a_reader());
                auto forwarded_stream = Streams::ReadableStream::create(realm);
                auto end_of_body_steps = GC::create_function(GC::Heap::the(), GC::captures(process_response_end_of_body), [](auto& process_response_end_of_body) {
                    process_response_end_of_body->function()();
                });

                auto pull_algorithm = GC::create_function(GC::Heap::the(), GC::captures(realm, reader, forwarded_stream, end_of_body_steps), [](auto& realm, auto& reader, auto& forwarded_stream, auto& end_of_body_steps) {
                    auto promise = WebIDL::create_promise(realm);
                    reader->read_a_chunk(*GC::Heap::the().allocate<ResponseBodyForwardingReadRequest>(forwarded_stream, promise, end_of_body_steps));
                    return promise;
                });

                auto cancel_algorithm = GC::create_function(GC::Heap::the(), GC::captures(reader), [](auto& reader, JS::Value reason) {
                    return reader->cancel(reason);
                });

                forwarded_stream->set_up_with_byte_reading_support(realm, pull_algorithm, cancel_algorithm);
                body_view->set_stream(forwarded_stream);
            } else {
                // 1. Let transformStream be a new TransformStream.
                auto transform_stream = Streams::TransformStream::create(realm);

                // 2. Let identityTransformAlgorithm be an algorithm which, given chunk, enqueues chunk in transformStream.
                auto identity_transform_algorithm = GC::create_function(GC::Heap::the(), GC::captures(realm, transform_stream), [](auto& realm, auto& transform_stream, JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
                    if (auto result = transform_stream->enqueue(chunk); result.is_error())
                        return WebIDL::create_rejected_promise_from_exception(realm, result.release_error());

                    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
                });

                // 3. Set up transformStream with transformAlgorithm set to identityTransformAlgorithm and flushAlgorithm set
                //    to processResponseEndOfBody.
                auto flush_algorithm = GC::create_function(GC::Heap::the(), GC::captures(realm, process_response_end_of_body), [](auto& realm, auto& process_response_end_of_body) -> GC::Ref<WebIDL::Promise> {
                    process_response_end_of_body->function()();
                    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
                });
                transform_stream->set_up(realm, identity_transform_algorithm, flush_algorithm);

                // 4. Set internalResponse’s body’s stream to the result of internalResponse’s body’s stream piped through transformStream.
                body_view->set_stream(body_view->stream()->piped_through(transform_stream));
            }
            break;
        }
        case Engine::BodyIntent::DrainAndDiscard: {
            // AD-HOC: No algorithm reads this body, so the pipe above would never flush, and a body larger than the
            //         transport's credit would stall its transfer. Read the body to its end and drop it, then run
            //         processResponseEndOfBody as the flush would have.
            VERIFY(!algorithms->process_response_consume_body());
            auto discard_chunk = GC::create_function(GC::Heap::the(), [](ByteBuffer) { });
            auto end_of_body = GC::create_function(GC::Heap::the(), GC::captures(process_response_end_of_body), [](auto& process_response_end_of_body) {
                process_response_end_of_body->function()();
            });
            auto discard_error = GC::create_function(GC::Heap::the(), [](JS::Value) { });
            body_view->incrementally_read(realm, discard_chunk, end_of_body, discard_error, controller->task_destination());
            break;
        }
        case Engine::BodyIntent::SyncAccumulate:
            // AD-HOC: processResponseEndOfBody runs once the direct read of the stream below completes (see
            //         read_body_in_parallel above).
            break;
        }
    }

    // 9. If fetchParams’s process response consume body is non-null, then:
    if (algorithms->process_response_consume_body()) {
        // 1. Let processBody given nullOrBytes be this step: run fetchParams’s process response consume body given
        //    response and nullOrBytes.
        auto process_body = GC::create_function(GC::Heap::the(), GC::captures(algorithms, internal_response, response_view), [](auto& algorithms, auto& internal_response, auto& response_view, ByteBuffer bytes) {
            if (auto body = internal_response->body(); body.has_value()) {
                if (auto source = body->source(); source.has_value()) {
                    algorithms->process_response_consume_body()->function()(response_view, *source);
                    return;
                }
            }
            // A body the transport delivered as slices of one mapping keeps that mapping instead of the bytes read.
            if (auto const& network_transport = internal_response->network_transport(); network_transport.has_value()) {
                if (auto whole_body = network_transport->body_channel->whole_body(); whole_body.has_value()) {
                    algorithms->process_response_consume_body()->function()(response_view, whole_body.release_value());
                    return;
                }
            }
            algorithms->process_response_consume_body()->function()(response_view, Core::ImmutableBytes::adopt(move(bytes)));
        });

        // 2. Let processBodyError be this step: run fetchParams’s process response consume body given response and
        //    failure.
        auto process_body_error = GC::create_function(GC::Heap::the(), GC::captures(algorithms, response_view, controller, setup_report_timing_steps), [](auto& algorithms, auto& response_view, auto& controller, auto& setup_report_timing_steps, JS::Value) {
            // AD-HOC: See comment on setup_report_timing_steps above.
            setup_report_timing_steps->function()(HighResolutionTime::unsafe_shared_current_time());
            // NB: A failed read does not always close or error the body's stream, which would otherwise end this.
            end_rendering_pending_operation(controller.ptr());
            algorithms->process_response_consume_body()->function()(response_view, Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag {});
        });

        // 3. If internalResponse's body is null, then queue a fetch task to run processBody given null, with
        //    fetchParams’s task destination.
        if (!internal_response->body().has_value()) {
            // AD-HOC: For a sync XHR's body, run processBody directly (see read_body_in_parallel above;
            //         processResponseEndOfBody already ran, right after it).
            if (read_body_in_parallel) {
                algorithms->process_response_consume_body()->function()(response_view, Empty {});
            } else {
                controller->queue_fetch_task(GC::create_function(GC::Heap::the(), GC::captures(algorithms, response_view), [](auto& algorithms, auto& response_view) {
                    // NB: We have to provide fully_read a callback which accepts a ByteBuffer. Since that's not
                    //     nullable, we just invoke process_response_consume_body with a null value manually here.
                    algorithms->process_response_consume_body()->function()(response_view, Empty {});
                }));
            }
        }
        // 4. Otherwise, fully read internalResponse body given processBody, processBodyError, and fetchParams’s task
        //    destination.
        // AD-HOC: For a sync XHR's body, read the stream directly, without the event loop (see the comment at
        //         read_body_in_parallel above).
        else if (read_body_in_parallel) {
            HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            auto success_steps = GC::create_function(GC::Heap::the(), GC::captures(process_body, process_response_end_of_body), [](auto& process_body, auto& process_response_end_of_body, ByteBuffer bytes) {
                // NB: processResponseEndOfBody runs first, as step 8's flush algorithm would have: It sets up the fetch
                //     controller's report timing steps, which the caller may invoke as soon as processBody has run.
                process_response_end_of_body->function()();
                process_body->function()(move(bytes));
            });

            // NB: A body that carries its full contents as an in-memory source (data: URLs, cached responses, and such)
            //     is read from that directly: Its stream may be populated thru the event loop (Blob::get_stream()
            //     enqueues from a queued global task, e.g.) — which can't happen while the loop is paused.
            auto const& network_transport = internal_response->network_transport();
            auto has_whole_body = network_transport.has_value() && network_transport->body_channel->whole_body().has_value();
            if (internal_response->body()->source().has_value() || has_whole_body) {
                // NB: process_body() reads the body's source itself and ignores the bytes passed here.
                success_steps->function()({});
                if (network_transport.has_value())
                    network_transport->handle->post_stop(controller->started_fetch_executor());
            } else {
                auto reader = body_view->stream()->get_a_reader();
                if (reader.is_exception()) {
                    auto throw_completion = WebIDL::exception_to_throw_completion(realm.vm(), realm, reader.release_error());
                    process_body_error->function()(throw_completion.release_value());
                } else {
                    reader.value()->read_all_bytes(success_steps, process_body_error);
                }
            }
        } else {
            body_view->fully_read(realm, process_body, process_body_error, controller->task_destination());
        }
    }
}

GC::Ptr<Infrastructure::Body> adopt_navigation_response_body(JS::Realm& realm, Infrastructure::Response& response, Optional<Utf16String> const& request_server_top_level_site, Requests::RequestTransferLeaseKey transfer_lease, RefPtr<Infrastructure::FetchTimingInfo> timing_info, GC::Ref<GC::Function<void(Optional<StringView> error)>> on_network_settled)
{
    if (!ResourceLoader::is_initialized())
        return {};

    // The UI process moved the response to the RequestServer of the given top-level site.
    auto request_client = ResourceLoader::the().request_client_for_top_level_site(request_server_top_level_site);
    if (!request_client)
        return {};

    auto request = request_client->adopt_request(transfer_lease.source_client_id, transfer_lease.source_request_id, Requests::RequestClient::TransferLease::Yes);
    if (!request)
        return {};

    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // Only the bytes the navigation sniffs are delivered until it decides what the response becomes.
    auto body_pump = NetworkBodyPump::create(nullptr, Engine::FetchBodyDeliveryGate::NavigationHold::Held, nullptr);
    body_pump->set_response(response);

    auto network_transport = Engine::NetworkTransportHandle::create(*request);
    body_pump->delivery_gate()->attach_transport(network_transport);
    auto& fetch_executor = *HTML::relevant_agent(realm.global_object()).fetch_executor;
    Engine::Command::NavigationBodyAdopted adopted { network_transport, body_pump->delivery_gate() };
    fetch_executor.post(move(adopted));

    // The observer hears the body's end at most once. A stop the agent requests is reported in the call, unless the
    // body ended first.
    auto network_settled = GC::make_root(on_network_settled);
    network_transport->set_on_stop_requested([network_settled] {
        network_settled->function()("Transferred navigation response was stopped"sv);
    });
    auto weak_network_transport = network_transport->make_weak_ptr<Engine::NetworkTransportHandle>();
    auto take_stopped_report = [weak_network_transport]() -> Function<void()> {
        auto handle = weak_network_transport.strong_ref();
        if (!handle)
            return {};
        return handle->take_on_stop_requested();
    };
    network_transport->set_on_teardown([body_pump, take_stopped_report](Requests::Request::TeardownReason reason) {
        body_pump->handle_network_teardown(reason);
        auto report_stopped = take_stopped_report();
        // A body that moves to another owner is reported there.
        if (reason == Requests::Request::TeardownReason::Stopped && report_stopped)
            report_stopped();
    });

    // Until a stop the agent requested reaches the request, the body takes none of its data, and ends as stopped if the
    // request finishes first.
    auto is_stop_requested = [weak_network_transport] {
        auto handle = weak_network_transport.strong_ref();
        return handle && handle->is_stop_requested();
    };

    auto cross_origin_isolated_capability = Bindings::principal_host_defined_environment_settings_object(realm).cross_origin_isolated_capability();
    request->set_unbuffered_request_callbacks(
        [](NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String> const&, Requests::CacheState) { },
        [body_pump, is_stop_requested](Requests::ResponseData data) {
            if (is_stop_requested())
                return;
            body_pump->handle_network_data(move(data));
        },
        [body_pump](Core::ImmutableBytes data) {
            body_pump->set_cached_response_body(move(data));
        },
        [body_pump, timing_info, cross_origin_isolated_capability, network_settled, is_stop_requested, take_stopped_report](u64, Requests::RequestTimingInfo const& request_timing_info, Optional<Requests::NetworkError> network_error) {
            // A stop requested after the body ended reports nothing.
            (void)take_stopped_report();
            if (is_stop_requested()) {
                body_pump->handle_network_teardown(Requests::Request::TeardownReason::Stopped);
                return;
            }
            if (network_error.has_value()) {
                network_settled->function()("Transferred navigation response failed"sv);
                body_pump->handle_network_error("Transferred navigation response failed"sv);
            } else {
                // AD-HOC: Nothing reports timing for a navigation fetch, so record the network phases and end time
                //         here for the Document's navigation timing entry.
                if (timing_info) {
                    timing_info->update_final_timings(request_timing_info, cross_origin_isolated_capability);
                    timing_info->set_end_time(HighResolutionTime::unsafe_shared_current_time());
                }
                body_pump->handle_network_complete(request_timing_info.encoded_body_size);
                network_settled->function()({});
            }
        });

    response.set_body(Infrastructure::ResponseBody { body_pump->channel(), {} });
    response.set_network_transport({
        .handle = move(network_transport),
        .body_delivery_gate = body_pump->delivery_gate(),
        .body_channel = body_pump->channel(),
    });
    return create_body_view(realm, response);
}

void clear_http_memory_cache()
{
    clear_http_memory_cache_entries();
    HTML::CompiledScriptCache::the().clear();
}

class ResourceLoaderKnownHSTSHostProvider final : public Engine::KnownHSTSHostProvider {
public:
    virtual Optional<bool> known_answer(String const& host) const override
    {
        if (ResourceLoader::is_preloaded_hsts_host(host))
            return true;
        if (!ResourceLoader::is_initialized())
            return false;
        return {};
    }

    virtual void look_up(Infrastructure::FetchParams const& fetch_params, String const& host, Function<void(bool is_known)> on_answer) override
    {
        auto network_isolation_key = Infrastructure::determine_the_network_partition_key(fetch_params.request());
        ResourceLoader::the().look_up_known_hsts_host(network_isolation_key, host, move(on_answer));
    }
};

NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor(NonnullRefPtr<Engine::NetworkActivityObservers const> network_activity_observers)
{
    Engine::FetchExecutorServices services {
        .event_loop = Core::EventLoop::current_weak(),
        .known_hsts_host_provider = make_ref_counted<ResourceLoaderKnownHSTSHostProvider>(),
    };
    return Engine::InlineFetchExecutor::create(move(services), make<FetchEngine>(move(network_activity_observers), make<Engine::FetchEngineRecords::Agent>()));
}

}
