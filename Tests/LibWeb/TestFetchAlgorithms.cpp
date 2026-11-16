/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <AK/JsonObject.h>
#include <AK/Random.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibJS/Runtime/Error.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/EngineEvent.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/FetchEventSink.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Fetching/NetworkBodyPump.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebIDL/DOMException.h>

using Web::Fetch::Engine::EngineCommand;
using Web::Fetch::Engine::EngineCommandSink;
using Web::Fetch::Engine::FetchBodyDeliveryGate;
using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::NetworkTransportHandle;
using Web::Fetch::Engine::WholeBodyTransport;
using Web::Fetch::Engine::Command::TransportControl;
using Web::Fetch::Engine::Event::EndOfBody;
using Web::Fetch::Engine::Event::NetworkActivity;
using Web::Fetch::Engine::Event::ReadRequestBodyStream;
using Web::Fetch::Engine::Event::RequestBodyEndOfBody;
using Web::Fetch::Engine::Event::RequestBodyProgress;
using Web::Fetch::Engine::Event::RequestBodySentFromSource;
using Web::Fetch::Engine::Event::RequestTransmissionEnded;
using Web::Fetch::Fetching::FetchEventSink;
using Web::Fetch::Fetching::NetworkBodyPump;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchParams;
using Web::Fetch::Infrastructure::Request;
using Web::Fetch::Infrastructure::Response;
using Web::Fetch::Infrastructure::ResponseView;

namespace {

// What the algorithms of a fetch saw, which the test reads once they have run.
struct AlgorithmCalls : RefCounted<AlgorithmCalls> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<StringView> calls;
    Vector<GC::Root<ResponseView>> responses;
    Optional<ByteBuffer> consumed_bytes;
    size_t call_count { 0 };
    bool consumed_body { false };
};

struct FetchHarness {
    FetchHarness()
        : document(create_test_document())
        , realm(document->relevant_settings_object().realm())
    {
        // Each case has an event loop of its own, which the window agent's fetches have to run on.
        auto& agent = Web::HTML::surrounding_agent();
        agent.fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor(agent.network_activity_observers);
    }

    // An unknown scheme: scheme fetch returns a network error, which has no body.
    NonnullRefPtr<Request> create_request() const
    {
        return create_request_for("unknown-scheme:network-error"sv);
    }

    NonnullRefPtr<Request> create_request_for(StringView url_string) const
    {
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        auto request = Request::create();
        request->set_url_list({ url.release_value() });
        request->set_client(document->relevant_settings_object().address());
        return request;
    }

    // Fetches from the harness's document, or from the given one.
    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch(Request& request, FetchAlgorithms::Input input, GC::Ptr<Web::DOM::Document> from_document = {}) const
    {
        return fetch_with(request, FetchAlgorithms::create(move(input)), from_document);
    }

    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch_with(Request& request, FetchAlgorithms const& algorithms, GC::Ptr<Web::DOM::Document> from_document = {}) const
    {
        auto& settings = (from_document ? *from_document : *document).relevant_settings_object();
        request.set_client(settings.address());
        Web::HTML::TemporaryExecutionContext execution_context { settings.realm(), Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        return Web::Fetch::Fetching::fetch(settings.realm(), request, algorithms);
    }

    // Runs the fetch's steps and the tasks they queue until the condition holds, or for ten seconds.
    void run_tasks_until(GC::Ref<GC::Function<bool()>> condition) const
    {
        auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [] { });
        deadline->start();
        Web::HTML::main_thread_event_loop().spin_until(GC::create_function(realm->heap(), GC::captures(condition, deadline->make_weak_ptr<Core::Timer>()), [](auto& condition, auto& deadline) {
            return condition->function()() || !deadline || !deadline->is_active();
        }));
    }

    GC::Ref<Web::DOM::Document> document;
    GC::Ref<JS::Realm> realm;
};

}

TEST_CASE(each_algorithm_gets_the_same_response_in_order)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append("process response"sv);
        record->responses.append(GC::make_root(response));
    });
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append("process response end-of-body"sv);
        record->responses.append(GC::make_root(response));
    });
    input.process_response_consume_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response, FetchAlgorithms::BodyBytes body) {
        record->calls.append("process response consume body"sv);
        record->responses.append(GC::make_root(response));
        EXPECT(body.has<Empty>());
    });

    auto request = harness.create_request();
    (void)harness.fetch(request, move(input));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 3; }));

    auto const& calls = record->calls;
    auto const& responses = record->responses;
    EXPECT_EQ(calls, (Vector<StringView> { "process response"sv, "process response end-of-body"sv, "process response consume body"sv }));
    VERIFY(responses.size() == 3);
    EXPECT(responses[0]->is_network_error());
    EXPECT_EQ(responses[1].ptr(), responses[0].ptr());
    EXPECT_EQ(responses[2].ptr(), responses[0].ptr());
}

TEST_CASE(each_algorithm_gets_the_same_response_with_a_body_in_order)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append("process response"sv);
        record->responses.append(GC::make_root(response));
    });
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append("process response end-of-body"sv);
        record->responses.append(GC::make_root(response));
    });
    input.process_response_consume_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response, FetchAlgorithms::BodyBytes body) {
        record->calls.append("process response consume body"sv);
        record->responses.append(GC::make_root(response));
        if (auto const* bytes = body.get_pointer<Core::ImmutableBytes>())
            record->consumed_bytes = MUST(ByteBuffer::copy(bytes->bytes()));
    });

    auto request = harness.create_request_for("data:text/plain,hello"sv);
    (void)harness.fetch(request, move(input));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 3; }));

    auto const& calls = record->calls;
    auto const& responses = record->responses;
    auto const& consumed_bytes = record->consumed_bytes;
    EXPECT_EQ(calls, (Vector<StringView> { "process response"sv, "process response end-of-body"sv, "process response consume body"sv }));
    VERIFY(responses.size() == 3);
    EXPECT(!responses[0]->is_network_error());
    EXPECT_EQ(responses[1].ptr(), responses[0].ptr());
    EXPECT_EQ(responses[2].ptr(), responses[0].ptr());
    VERIFY(consumed_bytes.has_value());
    EXPECT_EQ(*consumed_bytes, MUST(ByteBuffer::copy("hello"sv.bytes())));
}

TEST_CASE(a_fetch_stopped_before_its_response_runs_no_algorithm_but_its_request_is_done)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) { ++record->call_count; });
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) { ++record->call_count; });
    input.process_response_consume_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { ++record->call_count; });

    auto request = harness.create_request();
    auto controller = harness.fetch(request, move(input));
    controller->stop_fetch();
    harness.run_tasks_until(GC::create_function(heap, GC::captures(request), [](auto& request) { return request->done(); }));

    EXPECT_EQ(record->call_count, 0u);
    EXPECT(request->done());
}

TEST_CASE(a_stop_leaves_the_algorithms_for_the_next_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) { ++record->call_count; });
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) { ++record->call_count; });
    input.process_response_consume_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { ++record->call_count; });
    auto algorithms = FetchAlgorithms::create(move(input));

    auto stopped_request = harness.create_request();
    harness.fetch_with(stopped_request, algorithms)->stop_fetch();
    harness.run_tasks_until(GC::create_function(heap, GC::captures(stopped_request), [](auto& stopped_request) { return stopped_request->done(); }));
    EXPECT_EQ(record->call_count, 0u);

    // A stop empties the stopped fetch's algorithms, not the caller's.
    auto request = harness.create_request();
    (void)harness.fetch_with(request, algorithms);
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->call_count == 3; }));
    EXPECT_EQ(record->call_count, 3u);
}

namespace {

// What reached the steps a test set up, which the test reads once they have run.
struct ReachedSteps : RefCounted<ReachedSteps> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<ByteString> steps;
    bool marker_reached { false };
};

// The commands posted to an engine that applies none of them, for the test to hand on.
struct PostedCommands : RefCounted<PostedCommands> {
    Vector<EngineCommand> commands;
};

ByteString describe_transport_control(TransportControl::Operation const& operation, Web::Fetch::Engine::RequestId request_id, Web::Fetch::Engine::TransportEpoch transport_epoch)
{
    auto description = operation.visit(
        [](TransportControl::StopTransport) { return ByteString { "stop transport"sv }; },
        [](TransportControl::ReleaseNavigationHold) { return ByteString { "release navigation hold"sv }; },
        [](TransportControl::ReleaseNavigationHoldUpTo const& release) { return ByteString::formatted("release navigation hold up to {}", release.byte_count); });
    return ByteString::formatted("{} of request {} of epoch {}", description, request_id.value(), transport_epoch.value());
}

// Records each command that reaches the fetch engine, and applies none of them.
class CommandRecordingEngine final : public EngineCommandSink {
public:
    explicit CommandRecordingEngine(NonnullRefPtr<ReachedSteps> reached, RefPtr<PostedCommands> posted_commands = {})
        : m_reached(move(reached))
        , m_posted_commands(move(posted_commands))
    {
    }

    virtual void handle_command(EngineCommand command) override
    {
        command.visit(
            [&](Web::Fetch::Engine::Command::Abort const& abort) { m_reached->steps.append(ByteString::formatted("abort {}", abort.fetch_id.value())); },
            [&](Web::Fetch::Engine::Command::Terminate const& terminate) { m_reached->steps.append(ByteString::formatted("terminate {}", terminate.fetch_id.value())); },
            [&](Web::Fetch::Engine::Command::Stop const& stop) { m_reached->steps.append(ByteString::formatted("stop {}", stop.fetch_id.value())); },
            [&](Web::Fetch::Engine::Command::NavigationBodyAdopted const&) { m_reached->steps.append("navigation body adopted"); },
            [&](TransportControl const& control) { m_reached->steps.append(describe_transport_control(control.operation, control.request_id, control.transport_epoch)); },
            [](auto const&) { FAIL("unexpected command"); });
        if (m_posted_commands)
            m_posted_commands->commands.append(move(command));
    }

private:
    NonnullRefPtr<ReachedSteps> m_reached;
    RefPtr<PostedCommands> m_posted_commands;
};

// A fetch that has run to its end, with a response body whose transport the fetch's engine record keeps.
struct FetchWithBody {
    GC::Root<FetchController> controller;
    NonnullRefPtr<FetchParams> fetch_params;
    GC::Root<NetworkBodyPump> body_pump;
    NonnullRefPtr<WholeBodyTransport> transport;
};

}

static FetchWithBody fetch_with_a_body(FetchHarness const& harness)
{
    auto request = harness.create_request();
    auto controller = GC::make_root(harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard }));
    auto fetch_params = controller->original_fetch_params();
    VERIFY(fetch_params);
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(request), [](auto& request) { return request->done(); }));

    auto body_pump = GC::make_root(NetworkBodyPump::create(fetch_params, FetchBodyDeliveryGate::NavigationHold::None, nullptr));
    auto transport = Web::Fetch::Fetching::create_whole_body_transport(*body_pump->delivery_gate(), *body_pump->channel());
    fetch_params->set_body_transport(*transport);
    return { move(controller), fetch_params.release_nonnull(), move(body_pump), move(transport) };
}

// Hands the transport its body, as the network fetch does once the response's headers arrive.
static void deliver_body(FetchWithBody& fetch, WholeBodyTransport::OnData on_data)
{
    fetch.transport->attach_to(*fetch.body_pump->delivery_gate());
    auto body = Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("hello"sv.bytes())));
    fetch.transport->set_body(move(body), move(on_data));
}

// Runs the deferred invocations queued so far, then those that they queue.
static void run_queued_invocations(FetchHarness const& harness)
{
    auto reached = make_ref_counted<ReachedSteps>();
    Core::deferred_invoke([reached] {
        Core::deferred_invoke([reached] { reached->marker_reached = true; });
    });
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(reached), [](auto& reached) { return reached->marker_reached; }));
}

TEST_CASE(each_stop_of_a_started_fetch_reaches_the_engine_for_its_fetch_id)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto reached = make_ref_counted<ReachedSteps>();
    Web::Fetch::Engine::FetchExecutorServices services { .event_loop = Core::EventLoop::current_weak() };
    auto fetch_executor = Web::Fetch::Engine::InlineFetchExecutor::create(move(services), make<CommandRecordingEngine>(reached));

    auto& registry = Web::HTML::surrounding_agent().fetch_controller_registry;
    auto create_controller = [&](Optional<u64> fetch_id) {
        auto controller = GC::make_root(FetchController::create(registry));
        if (fetch_id.has_value())
            controller->set_started_fetch(Web::Fetch::Engine::FetchId { *fetch_id }, *fetch_executor);
        return controller;
    };
    auto stopped = create_controller(1);
    auto aborted = create_controller(2);
    auto terminated = create_controller(3);
    auto never_started = create_controller({});
    // A navigation's manual redirect starts another fetch with the same controller.
    auto redirected = create_controller(4);
    redirected->set_started_fetch(Web::Fetch::Engine::FetchId { 5 }, *fetch_executor);

    stopped->stop_fetch();
    {
        Web::HTML::TemporaryExecutionContext execution_context { *harness.realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        aborted->abort(*harness.realm, {});
        never_started->abort(*harness.realm, {});
    }
    terminated->terminate();
    never_started->terminate();
    redirected->stop_fetch();
    run_queued_invocations(harness);

    EXPECT_EQ(reached->steps, (Vector<ByteString> { "stop 1", "abort 2", "terminate 3", "stop 5" }));
}

TEST_CASE(a_stop_reaches_the_body_transport_of_the_fetch_before_deliveries_queued_after_it)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto stopped = fetch_with_a_body(harness);
    auto aborted = fetch_with_a_body(harness);
    auto terminated = fetch_with_a_body(harness);

    stopped.controller->stop_fetch();
    {
        Web::HTML::TemporaryExecutionContext execution_context { *harness.realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        aborted.controller->abort(*harness.realm, {});
    }
    terminated.controller->terminate();

    auto reached = make_ref_counted<ReachedSteps>();
    for (auto* fetch : { &stopped, &aborted, &terminated }) {
        deliver_body(*fetch, [reached](Requests::ResponseData) { reached->steps.append("data"); });
        fetch->transport->when_delivered([reached] { reached->steps.append("end"); });
    }
    run_queued_invocations(harness);

    EXPECT(reached->steps.is_empty());
    for (auto* fetch : { &stopped, &aborted, &terminated })
        EXPECT(fetch->body_pump->channel()->peek_up_to(0).state.has<FetchByteChannel::ConsumerCancelled>());
}

TEST_CASE(body_bytes_queued_before_a_stop_are_not_delivered)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto fetch = fetch_with_a_body(harness);
    deliver_body(fetch, [body_pump = fetch.body_pump](Requests::ResponseData data) { body_pump->handle_network_data(move(data)); });
    fetch.controller->stop_fetch();
    run_queued_invocations(harness);

    auto buffered = fetch.body_pump->channel()->peek_up_to(5);
    EXPECT(buffered.taken.is_empty());
    EXPECT(buffered.state.has<FetchByteChannel::ConsumerCancelled>());
}

static NEVER_INLINE void start_fetch_without_holding_its_controller(FetchHarness const& harness, Request& request, FetchAlgorithms::Input input)
{
    (void)harness.fetch(request, move(input));
}

static NEVER_INLINE void run_a_fetch_whose_controller_nothing_holds()
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response_consume_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { record->consumed_body = true; });

    // A worker's script fetch has no fetch record, so only its own steps hold its controller.
    auto request = harness.create_request();
    request->set_destination(Request::Destination::Worker);
    start_fetch_without_holding_its_controller(harness, request, move(input));
    GC::scrub_stack_for_testing();
    harness.run_tasks_until(GC::create_function(heap, GC::captures(harness.realm, record), [](auto& realm, auto& record) {
        realm->heap().collect_garbage();
        return record->consumed_body;
    }));

    EXPECT(record->consumed_body);
}

TEST_CASE(a_fetch_whose_controller_nothing_holds_still_runs_its_algorithms)
{
    GC::scrub_stack_for_testing();
    run_a_fetch_whose_controller_nothing_holds();
}

namespace {

// What an upload's algorithms and tasks did, which the test reads once they have run.
struct UploadCalls : RefCounted<UploadCalls> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<ByteString> calls;
    bool later_task_ran { false };
};

}

// The algorithms of an upload, which record each call.
static FetchAlgorithms::Input upload_algorithms(GC::Heap& heap, NonnullRefPtr<UploadCalls> const& record)
{
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
    input.process_request_body_chunk_length = GC::create_function(heap, GC::captures(record), [](auto& record, u64 length) { record->calls.append(ByteString::formatted("chunk {}", length)); });
    input.process_request_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record) { record->calls.append("end of body"); });
    return input;
}

// A request body that only its stream has, which the steps set up before the stream is read.
static FetchAlgorithms::RequestBodyReader create_request_body_reader(JS::Realm& realm, Function<void(Web::Streams::ReadableStream&)> set_up_stream)
{
    Web::HTML::TemporaryExecutionContext execution_context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto stream = Web::Streams::ReadableStream::create(realm);
    stream->set_up_with_byte_reading_support(realm);
    set_up_stream(stream);
    auto reader = MUST(stream->get_a_reader());
    return { Web::Fetch::Infrastructure::Body::create(stream), reader };
}

TEST_CASE(a_request_body_runs_its_algorithms_in_fetch_tasks_until_its_end)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto request = harness.create_request();
    auto controller = harness.fetch(request, upload_algorithms(heap, record));

    FetchEventSink event_sink { controller };
    event_sink.handle_event(RequestBodyProgress { .length = 3 });
    event_sink.handle_event(RequestBodyProgress { .length = 4 });
    event_sink.handle_event(RequestBodyEndOfBody {});
    EXPECT(record->calls.is_empty());

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 3; }));
    EXPECT_EQ(record->calls, (Vector<ByteString> { "chunk 3", "chunk 4", "end of body" }));
}

TEST_CASE(a_canceled_fetch_runs_no_algorithm_of_a_non_null_request_body)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto request = harness.create_request();
    auto controller = harness.fetch(request, upload_algorithms(heap, record));

    FetchEventSink event_sink { controller };
    event_sink.handle_event(RequestBodyProgress { .length = 3 });
    event_sink.handle_event(RequestBodyEndOfBody {});
    controller->terminate();

    // A task queued after the request body's tasks runs after them.
    Web::Fetch::Infrastructure::queue_fetch_task(controller, controller->task_destination(), GC::create_function(heap, GC::captures(record), [](auto& record) { record->later_task_ran = true; }));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->later_task_ran; }));

    EXPECT(record->later_task_ran);
    EXPECT(record->calls.is_empty());
}

TEST_CASE(the_end_of_a_null_request_body_runs_for_a_fetch_canceled_after_it_is_queued)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto request = harness.create_request();
    auto controller = harness.fetch(request, upload_algorithms(heap, record));

    FetchEventSink event_sink { controller };
    event_sink.handle_event(RequestBodyEndOfBody { .body_is_null = true });
    controller->terminate();
    EXPECT(record->calls.is_empty());

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return !record->calls.is_empty(); }));
    EXPECT_EQ(record->calls, (Vector<ByteString> { "end of body" }));
}

TEST_CASE(a_request_body_without_a_source_is_read_from_its_stream_until_its_end)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto input = upload_algorithms(heap, record);
    input.request_body_reader = create_request_body_reader(*harness.realm, [](auto& stream) {
        MUST(stream.enqueue_bytes("hello"sv.bytes()));
        stream.close();
    });
    auto request = harness.create_request();
    auto controller = harness.fetch(request, move(input));

    FetchEventSink event_sink { controller };
    event_sink.handle_event(ReadRequestBodyStream {});

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 2; }));
    EXPECT_EQ(record->calls, (Vector<ByteString> { "chunk 5", "end of body" }));
}

TEST_CASE(an_error_from_a_request_body_stream_aborts_or_terminates_the_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto& realm = *harness.realm;

    JS::Value abort_error;
    JS::Value type_error;
    {
        Web::HTML::TemporaryExecutionContext execution_context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        abort_error = Web::throw_completion(realm, Web::WebIDL::AbortError::create("The body was aborted"_utf16)).value();
        type_error = JS::TypeError::create(realm, "The body failed"sv);
    }

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto fetch_with_a_body_stream_errored_with = [&](JS::Value error) {
        auto input = upload_algorithms(heap, record);
        input.request_body_reader = create_request_body_reader(realm, [error](auto& stream) { stream.error(error); });
        auto request = harness.create_request();
        auto controller = harness.fetch(request, move(input));
        FetchEventSink { controller }.handle_event(ReadRequestBodyStream {});
        return controller;
    };
    auto aborted = fetch_with_a_body_stream_errored_with(abort_error);
    auto terminated = fetch_with_a_body_stream_errored_with(type_error);

    harness.run_tasks_until(GC::create_function(heap, GC::captures(aborted, terminated), [](auto& aborted, auto& terminated) {
        return aborted->state() != FetchController::State::Ongoing && terminated->state() != FetchController::State::Ongoing;
    }));
    EXPECT_EQ(aborted->state(), FetchController::State::Aborted);
    EXPECT_EQ(terminated->state(), FetchController::State::Terminated);
    EXPECT(record->calls.is_empty());
}

TEST_CASE(the_stream_of_a_request_body_sent_from_its_source_is_read_to_its_end_and_runs_no_algorithm)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto fetch_with_a_body_stream = [&](NonnullRefPtr<UploadCalls> const& record) {
        auto input = upload_algorithms(heap, record);
        input.request_body_reader = create_request_body_reader(*harness.realm, [](auto& stream) {
            MUST(stream.enqueue_bytes("hel"sv.bytes()));
            MUST(stream.enqueue_bytes("lo"sv.bytes()));
            stream.close();
        });
        auto request = harness.create_request();
        return harness.fetch(request, move(input));
    };
    auto drained = make_ref_counted<UploadCalls>();
    auto drained_controller = fetch_with_a_body_stream(drained);
    auto drained_stream = drained_controller->algorithms()->request_body_reader()->body->stream();
    FetchEventSink { drained_controller }.handle_event(RequestBodySentFromSource {});

    auto read = make_ref_counted<UploadCalls>();
    auto read_controller = fetch_with_a_body_stream(read);
    FetchEventSink { read_controller }.handle_event(ReadRequestBodyStream {});

    // The read stream's last task runs after every task of the drained stream.
    harness.run_tasks_until(GC::create_function(heap, GC::captures(read), [](auto& read) { return read->calls.size() == 3; }));
    EXPECT_EQ(read->calls, (Vector<ByteString> { "chunk 3", "chunk 2", "end of body" }));
    EXPECT(drained_stream->is_closed());
    EXPECT(drained->calls.is_empty());
}

namespace {

// A load over HTTP without a RequestServer fails as it starts, so the fetch sends no network request.
struct ResourceLoaderWithoutRequestServer {
    explicit ResourceLoaderWithoutRequestServer(GC::Heap& heap)
    {
        Web::ResourceLoader::initialize(heap);
    }

    ~ResourceLoaderWithoutRequestServer()
    {
        Web::ResourceLoader::shut_down();
    }
};

// What a RequestServer the test stands in for has been asked to start.
struct StartedRequests : RefCounted<StartedRequests> {
    GC_HOLDS_NO_CELL_EDGES;

    IPC::Transport* request_server { nullptr };
    Vector<u64> ids;
    Vector<ByteString> methods;
};

// A RequestServer client of the agent's resource loader, whose RequestServer end of the connection the test holds and
// answers for.
class StandInRequestServer {
public:
    explicit StandInRequestServer(FetchHarness const& harness)
        : m_harness(harness)
    {
        Web::ResourceLoader::initialize(harness.realm->heap());
        auto pair = MUST(IPC::Transport::create_paired());
        m_client = make_ref_counted<Requests::RequestClient>(move(pair.local), 1);
        Web::ResourceLoader::the().set_client({}, *m_client);
        m_request_server = MUST(pair.remote_handle.create_transport());
        m_started->request_server = m_request_server.ptr();
    }

    ~StandInRequestServer()
    {
        // The client can hear of its RequestServer's death after the resource loader is gone.
        m_client->on_request_server_died = nullptr;
        Web::ResourceLoader::shut_down();
    }

    // Runs the agent's tasks until another request with the method has started, and returns its id.
    u64 wait_for_request(StringView method)
    {
        // The RequestServer end is not part of the event loop, so keep the loop from sleeping until it has news.
        auto wake_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { });
        wake_timer->start();

        auto expected = ByteString { method };
        m_harness.run_tasks_until(GC::create_function(m_harness.realm->heap(), GC::captures(m_started, m_taken_count, expected), [](auto& started, auto& taken_count, auto& expected) {
            (void)started->request_server->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
                auto message = MUST(RequestServerEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
                if (message->message_id() != Messages::RequestServer::StartRequest::static_message_id())
                    return;
                auto const& start = static_cast<Messages::RequestServer::StartRequest&>(*message);
                started->ids.append(start.request_id());
                started->methods.append(start.method());
            });
            for (size_t index = taken_count; index < started->methods.size(); ++index) {
                if (started->methods[index] == expected)
                    return true;
            }
            return false;
        }));
        for (size_t index = m_taken_count; index < m_started->methods.size(); ++index) {
            if (m_started->methods[index] == method) {
                m_taken_count = index + 1;
                return m_started->ids[index];
            }
        }
        VERIFY_NOT_REACHED();
    }

    void send(IPC::MessageBuffer message)
    {
        MUST(message.transfer_message(*m_request_server));
    }

    void send_headers(u64 request_id, u32 status_code, Vector<HTTP::Header> headers = {})
    {
        send(MUST(Messages::RequestClient::HeadersBecameAvailable::static_encode(request_id, URL::about_blank(), move(headers), status_code, Optional<String> {}, Requests::CacheState::NotCached, ByteString {})));
    }

    void send_finished(u64 request_id, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error = {})
    {
        send(MUST(Messages::RequestClient::RequestFinished::static_encode(request_id, 0, timing_info, network_error)));
    }

    void send_transferred(u64 request_id)
    {
        send(MUST(Messages::RequestClient::RequestTransferred::static_encode(request_id)));
    }

private:
    FetchHarness const& m_harness;
    RefPtr<Requests::RequestClient> m_client;
    OwnPtr<IPC::Transport> m_request_server;
    NonnullRefPtr<StartedRequests> m_started { make_ref_counted<StartedRequests>() };
    // The started requests a wait has already returned or passed over.
    size_t m_taken_count { 0 };
};

}

TEST_CASE(a_request_body_sent_without_a_network_request_is_one_chunk_before_the_response)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    ResourceLoaderWithoutRequestServer resource_loader { harness.realm->heap() };

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto input = upload_algorithms(heap, record);
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append(response->is_network_error() ? "network error" : "response");
    });

    auto request = harness.create_request_for("http://127.0.0.1:8000/"sv);
    request->set_method("POST");
    request->set_body(MUST(ByteBuffer::copy("hello"sv.bytes())));
    (void)harness.fetch(request, move(input));

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 3; }));
    EXPECT_EQ(record->calls, (Vector<ByteString> { "chunk 5", "end of body", "network error" }));
}

TEST_CASE(a_request_body_sent_without_a_network_request_has_its_stream_read_to_its_end)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    ResourceLoaderWithoutRequestServer resource_loader { harness.realm->heap() };

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto input = upload_algorithms(heap, record);
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append(response->is_network_error() ? "network error" : "response");
    });
    input.request_body_reader = create_request_body_reader(*harness.realm, [](auto& stream) {
        MUST(stream.enqueue_bytes("hello"sv.bytes()));
        stream.close();
    });
    auto stream = input.request_body_reader->body->stream();

    auto request = harness.create_request_for("http://127.0.0.1:8000/"sv);
    request->set_method("POST");
    request->set_body(MUST(ByteBuffer::copy("hello"sv.bytes())));
    (void)harness.fetch(request, move(input));

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 3; }));
    EXPECT_EQ(record->calls, (Vector<ByteString> { "chunk 5", "end of body", "network error" }));
    EXPECT(stream->is_closed());
}

TEST_CASE(a_null_request_body_ends_before_the_response)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    ResourceLoaderWithoutRequestServer resource_loader { harness.realm->heap() };

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<UploadCalls>();
    auto input = upload_algorithms(heap, record);
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append(response->is_network_error() ? "network error" : "response");
    });

    auto request = harness.create_request_for("http://127.0.0.1:8000/"sv);
    (void)harness.fetch(request, move(input));

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 2; }));
    EXPECT_EQ(record->calls, (Vector<ByteString> { "end of body", "network error" }));
}

namespace {

// A navigation body adopted from a RequestServer that the test stands in for. The commands posted to the agent's
// executor are recorded, and reach a fetch engine only when the test applies them.
class AdoptedNavigationBody {
public:
    explicit AdoptedNavigationBody(FetchHarness const& harness)
        : m_harness(harness)
        , m_request_server(harness)
    {
        auto& agent = Web::HTML::surrounding_agent();
        Web::Fetch::Engine::FetchExecutorServices services { .event_loop = Core::EventLoop::current_weak() };
        auto recording_engine = make<CommandRecordingEngine>(commands, m_posted_commands);
        agent.fetch_executor = Web::Fetch::Engine::InlineFetchExecutor::create(move(services), move(recording_engine));
        m_engine_executor = Web::Fetch::Fetching::create_inline_fetch_executor(agent.network_activity_observers);

        auto on_network_settled = GC::create_function(harness.realm->heap(), GC::captures(settled), [](auto& settled, Optional<StringView> error) {
            settled->steps.append(error.has_value() ? ByteString { *error } : ByteString { "completed"sv });
        });
        Requests::RequestTransferLeaseKey transfer_lease { .source_client_id = 2, .source_request_id = 3 };
        auto body = Web::Fetch::Fetching::adopt_navigation_response_body(*harness.realm, *response, {}, transfer_lease, nullptr, on_network_settled);
        VERIFY(body);
        m_body = GC::make_root(*body);
    }

    NetworkTransportHandle& handle() const { return *response->network_transport()->handle; }
    Web::Fetch::Engine::FetchExecutor& agent_executor() const { return *Web::HTML::surrounding_agent().fetch_executor; }
    FetchByteChannel::TakeResult peek_body(u64 limit) const { return response->network_transport()->body_channel->peek_up_to(limit); }

    // Sends the message as RequestServer, and runs the agent's tasks until the body's request has received it.
    void receive(IPC::MessageBuffer message)
    {
        m_request_server.send(move(message));
        auto marker = ByteString::formatted("received {}", ++m_received_count);
        auto request_id = handle().request_id().value();
        auto marker_message = MUST(Messages::RequestClient::HeadersBecameAvailable::static_encode(request_id, URL::about_blank(), Vector<HTTP::Header> {}, Optional<u32> {}, Optional<String> {}, Requests::CacheState::NotCached, marker));
        m_request_server.send(move(marker_message));

        auto weak_handle = handle().make_weak_ptr<NetworkTransportHandle>();
        m_harness.run_tasks_until(GC::create_function(m_harness.realm->heap(), GC::captures(weak_handle, marker), [](auto& weak_handle, auto& marker) {
            auto handle = weak_handle.strong_ref();
            return handle && handle->http_version() == marker;
        }));
    }

    void finish_request()
    {
        auto request_id = handle().request_id().value();
        receive(MUST(Messages::RequestClient::RequestFinished::static_encode(request_id, 0, Requests::RequestTimingInfo {}, Optional<Requests::NetworkError> {})));
    }

    // Hands the commands posted so far to a fetch engine, and runs them.
    void apply_posted_commands()
    {
        run_queued_invocations(m_harness);
        for (auto& command : m_posted_commands->commands)
            m_engine_executor->post(move(command));
        m_posted_commands->commands.clear();
        run_queued_invocations(m_harness);
    }

    ByteString describe(TransportControl::Operation const& operation) const
    {
        return describe_transport_control(operation, handle().request_id(), handle().transport_epoch());
    }

    NonnullRefPtr<ReachedSteps> commands { make_ref_counted<ReachedSteps>() };
    NonnullRefPtr<ReachedSteps> settled { make_ref_counted<ReachedSteps>() };
    NonnullRefPtr<Web::Fetch::Infrastructure::Response> response { Web::Fetch::Infrastructure::Response::create() };

private:
    FetchHarness const& m_harness;
    StandInRequestServer m_request_server;
    NonnullRefPtr<PostedCommands> m_posted_commands { make_ref_counted<PostedCommands>() };
    RefPtr<Web::Fetch::Engine::FetchExecutor> m_engine_executor;
    GC::Root<Web::Fetch::Infrastructure::Body> m_body;
    u64 m_received_count { 0 };
};

}

TEST_CASE(an_adopted_navigation_body_completes_when_its_request_finishes)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    AdoptedNavigationBody adopted { harness };

    adopted.finish_request();
    EXPECT_EQ(adopted.settled->steps, (Vector<ByteString> { "completed" }));
    EXPECT(adopted.peek_body(0).state.has<FetchByteChannel::Closed>());
    EXPECT_EQ(adopted.commands->steps, (Vector<ByteString> { "navigation body adopted" }));
}

TEST_CASE(a_stop_of_an_adopted_navigation_body_is_reported_as_it_is_requested_and_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    AdoptedNavigationBody adopted { harness };

    adopted.handle().post_stop(adopted.agent_executor());
    EXPECT_EQ(adopted.settled->steps, (Vector<ByteString> { "Transferred navigation response was stopped" }));

    // The stop reaches the request, whose teardown ends the body.
    adopted.apply_posted_commands();
    EXPECT_EQ(adopted.settled->steps, (Vector<ByteString> { "Transferred navigation response was stopped" }));
    EXPECT(adopted.peek_body(0).state.has<FetchByteChannel::ConsumerCancelled>());
    auto stop = adopted.describe(TransportControl::StopTransport {});
    EXPECT_EQ(adopted.commands->steps, (Vector<ByteString> { "navigation body adopted", stop }));
}

TEST_CASE(an_adopted_navigation_body_whose_request_finishes_before_a_requested_stop_reaches_it_ends_as_stopped_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    AdoptedNavigationBody adopted { harness };

    adopted.handle().post_stop(adopted.agent_executor());
    adopted.finish_request();
    EXPECT(adopted.peek_body(0).state.has<FetchByteChannel::ConsumerCancelled>());

    // The stop reaches the request once it has finished.
    adopted.apply_posted_commands();
    EXPECT_EQ(adopted.settled->steps, (Vector<ByteString> { "Transferred navigation response was stopped" }));
    EXPECT(adopted.peek_body(0).state.has<FetchByteChannel::ConsumerCancelled>());
}

TEST_CASE(a_stop_requested_after_an_adopted_navigation_body_ended_reports_nothing)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    AdoptedNavigationBody adopted { harness };

    adopted.finish_request();
    adopted.handle().post_stop(adopted.agent_executor());
    adopted.apply_posted_commands();
    EXPECT_EQ(adopted.settled->steps, (Vector<ByteString> { "completed" }));
    EXPECT(adopted.peek_body(0).state.has<FetchByteChannel::Closed>());
}

TEST_CASE(an_adopted_navigation_body_takes_none_of_its_data_once_its_stop_is_requested)
{
    auto const payload = "adopted body bytes"sv;
    auto const path = ByteString::formatted("{}/TestFetchAlgorithms-{:016x}", Core::StandardPaths::tempfile_directory(), get_random<u64>());
    {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew));
        MUST(file->write_until_depleted(payload.bytes()));
    }
    // A file that is still mapped cannot be removed on Windows, so this runs last, once the request has unmapped it.
    ScopeGuard remove_file = [&path] { MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed)); };

    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    AdoptedNavigationBody adopted { harness };

    // The body flows, as once its document is created.
    adopted.response->network_transport()->body_delivery_gate->release_hold();
    adopted.handle().post_stop(adopted.agent_executor());

    auto request_id = adopted.handle().request_id().value();
    auto file = IPC::File::adopt_file(MUST(Core::File::open(path, Core::File::OpenMode::Read)));
    adopted.receive(MUST(Messages::RequestClient::RequestBodyFileAvailable::static_encode(request_id, file, 0, payload.length())));
    auto buffered = adopted.peek_body(payload.length());
    EXPECT(buffered.taken.is_empty());
    EXPECT(buffered.state.has<FetchByteChannel::Open>());

    adopted.finish_request();
    run_queued_invocations(harness);
    EXPECT(adopted.peek_body(0).state.has<FetchByteChannel::ConsumerCancelled>());
}

TEST_CASE(the_holders_of_an_adopted_navigation_body_post_its_controls_to_the_agents_fetch_executor)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    AdoptedNavigationBody adopted { harness };

    adopted.response->resume_body_delivery(adopted.agent_executor());
    adopted.response->release_navigation_hold_up_to(adopted.agent_executor(), 512);
    EXPECT(Web::HTML::stop_body_delivery(NonnullRefPtr { adopted.handle() }));
    EXPECT_EQ(adopted.settled->steps, (Vector<ByteString> { "Transferred navigation response was stopped" }));

    run_queued_invocations(harness);
    auto release = adopted.describe(TransportControl::ReleaseNavigationHold {});
    auto release_up_to = adopted.describe(TransportControl::ReleaseNavigationHoldUpTo { .byte_count = 512 });
    auto stop = adopted.describe(TransportControl::StopTransport {});
    EXPECT_EQ(adopted.commands->steps, (Vector<ByteString> { "navigation body adopted", release, release_up_to, stop }));
}

static Requests::RequestTimingInfo finished_timing_info(i64 microseconds)
{
    return {
        .domain_lookup_start_microseconds = microseconds,
        .domain_lookup_end_microseconds = microseconds + 1000,
        .connect_start_microseconds = microseconds + 2000,
        .connect_end_microseconds = microseconds + 3000,
        .request_start_microseconds = microseconds + 4000,
        .response_start_microseconds = microseconds + 5000,
        .encoded_body_size = 1234,
    };
}

static FetchAlgorithms::Input recording_responses(GC::Heap& heap, NonnullRefPtr<AlgorithmCalls> const& record)
{
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->responses.append(GC::make_root(response));
    });
    return input;
}

static void run_until_responses(FetchHarness const& harness, NonnullRefPtr<AlgorithmCalls> const& record, size_t count)
{
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(record, count), [](auto& record, auto& count) { return record->responses.size() == count; }));
    VERIFY(record->responses.size() == count);
}

static void run_until_encoded_size(FetchHarness const& harness, NonnullRefPtr<Response const> const& response, u64 encoded_size)
{
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(response, encoded_size), [](auto& response, auto& encoded_size) { return response->body_info().encoded_size == encoded_size; }));
}

namespace {

// A fetch whose response has been handed over while its body still arrives from the stand-in RequestServer.
struct DeliveredBody {
    GC::Root<FetchController> controller;
    NonnullRefPtr<Response const> response;
    Web::Fetch::Engine::DeliveryId delivery_id;
    u64 request_id { 0 };
};

}

static DeliveredBody deliver_a_body(FetchHarness const& harness, StandInRequestServer& request_server, StringView url, Optional<Request::InitiatorType> initiator_type = {})
{
    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for(url);
    request->set_initiator_type(initiator_type);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200);
    run_until_responses(harness, record, 1);
    auto response = record->responses[0]->record()->unsafe_response();
    auto delivery_id = response->network_transport()->body_channel->delivery_id();
    VERIFY(delivery_id.has_value());
    return { move(controller), move(response), *delivery_id, request_id };
}

// Reports the end of the delivered body, as the engine would once the body had arrived whole.
static void report_end_of_body(DeliveredBody const& delivered)
{
    EndOfBody end_of_body {
        .delivery_id = delivered.delivery_id,
        .final_sizes = { .encoded_body_size = 1, .decoded_body_size = 1 },
        .end_time = 1,
    };
    FetchEventSink event_sink { *delivered.controller };
    event_sink.handle_event(move(end_of_body));
}

// A request whose response main fetch reads whole, to check its integrity, before handing it over.
static NonnullRefPtr<Request> create_request_checked_for_integrity(FetchHarness const& harness, StringView url)
{
    auto request = harness.create_request_for(url);
    request->set_mode(Request::Mode::CORS);
    // The digest of an empty body.
    request->set_integrity_metadata("sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="_utf16);
    return request;
}

TEST_CASE(a_body_that_ends_before_its_response_is_handed_over_gets_its_sizes_from_its_pump)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "http://127.0.0.1:8000/ended-before-handover"sv);
    (void)harness.fetch(request, recording_responses(harness.realm->heap(), record));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_finished(request_id, finished_timing_info(1000));
    run_until_responses(harness, record, 1);

    auto response = record->responses[0]->record()->unsafe_response();
    EXPECT_EQ(response->body_info().encoded_size, 1234u);
    EXPECT(!response->network_transport()->body_channel->delivery_id().has_value());
}

TEST_CASE(a_body_that_ends_after_its_response_is_handed_over_gets_its_sizes_from_its_end)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto delivered = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/ended-after-handover"sv);
    EXPECT_EQ(delivered.response->body_info().encoded_size, 0u);

    request_server.send_finished(delivered.request_id, finished_timing_info(1000));
    run_until_encoded_size(harness, delivered.response, 1234);
    EXPECT_EQ(delivered.response->body_info().encoded_size, 1234u);
}

TEST_CASE(a_body_that_ends_after_its_response_is_handed_over_reports_the_end_time_of_its_end)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto delivered = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/reported"sv, Request::InitiatorType::Fetch);
    report_end_of_body(delivered);

    auto agent_timing_info = delivered.controller->control_block().agent_timing_info();
    VERIFY(agent_timing_info);
    EXPECT_EQ(agent_timing_info->end_time(), 1);
    EXPECT_EQ(delivered.controller->control_block().end_time(), 1);
}

TEST_CASE(a_stopped_fetch_no_longer_awaits_the_end_of_its_body)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto delivered = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/stopped"sv);
    delivered.controller->stop_fetch();
    report_end_of_body(delivered);
    EXPECT_EQ(delivered.response->body_info().encoded_size, 0u);
}

TEST_CASE(a_body_that_fails_or_moves_away_reports_no_end)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto failed = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/failed"sv);
    auto transferred = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/transferred"sv);
    request_server.send_finished(failed.request_id, finished_timing_info(1000), Requests::NetworkError::IncompleteContent);
    request_server.send_transferred(transferred.request_id);
    auto failed_channel = failed.response->network_transport()->body_channel;
    auto transferred_channel = transferred.response->network_transport()->body_channel;
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(failed_channel, transferred_channel), [](auto& failed_channel, auto& transferred_channel) {
        return failed_channel->peek_up_to(0).state.template has<FetchByteChannel::Errored>()
            && transferred_channel->peek_up_to(0).state.template has<FetchByteChannel::Transferred>();
    }));

    EXPECT(failed_channel->peek_up_to(0).state.has<FetchByteChannel::Errored>());
    EXPECT(transferred_channel->peek_up_to(0).state.has<FetchByteChannel::Transferred>());
    EXPECT_EQ(failed.response->body_info().encoded_size, 0u);
    EXPECT_EQ(transferred.response->body_info().encoded_size, 0u);

    // Each fetch still awaits the end of its body.
    report_end_of_body(failed);
    report_end_of_body(transferred);
    EXPECT_EQ(failed.response->body_info().encoded_size, 1u);
    EXPECT_EQ(transferred.response->body_info().encoded_size, 1u);
}

namespace {

// The WebDriver BiDi network events that reached the event sink, by name and request id.
struct ReportedNetworkEvents : RefCounted<ReportedNetworkEvents> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<String> names;
    Vector<String> request_ids;
};

// A WebDriver BiDi session with the given events enabled, whose events reach the callback, until it goes.
class WebDriverBiDiSession {
public:
    using OnEvent = Function<void(Web::HTML::Navigable const&, String const&, JsonValue)>;

    WebDriverBiDiSession(Vector<String> enabled_events, OnEvent on_event)
    {
        Web::WebDriver::BiDi::set_event_sink({ .on_event = move(on_event), .on_navigation_event = {} });
        Web::WebDriver::BiDiSessionState session_state;
        session_state.is_active = true;
        session_state.enabled_events = move(enabled_events);
        Web::WebDriver::BiDi::set_session_state(move(session_state));
    }

    ~WebDriverBiDiSession()
    {
        Web::WebDriver::BiDi::set_session_state({});
        Web::WebDriver::BiDi::set_event_sink({});
    }
};

}

TEST_CASE(a_request_fetched_again_keeps_its_webdriver_bidi_request_id)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto events = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session {
        { "network.beforeRequestSent"_string, "network.fetchError"_string },
        [events](Web::HTML::Navigable const&, String const& name, JsonValue params) {
            events->names.append(name);
            events->request_ids.append(*params.as_object().get_object("request"sv)->get_string("request"sv));
        },
    };

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard };
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) { ++record->call_count; });
    auto algorithms = FetchAlgorithms::create(move(input));
    auto run_fetch = [&](Request& request) {
        auto expected_call_count = record->call_count + 1;
        (void)harness.fetch_with(request, algorithms);
        harness.run_tasks_until(GC::create_function(heap, GC::captures(record, expected_call_count), [](auto& record, auto& expected_call_count) { return record->call_count == expected_call_count; }));
        VERIFY(record->call_count == expected_call_count);
    };

    // With no RequestServer, an http: fetch reports its request, then a network error.
    auto request = harness.create_request_for("http://localhost/"sv);
    run_fetch(request);
    EXPECT_EQ(events->names, (Vector<String> { "network.beforeRequestSent"_string, "network.fetchError"_string }));
    VERIFY(events->request_ids.size() == 2);
    EXPECT_EQ(events->request_ids[1], events->request_ids[0]);

    // Fetched again, the request keeps its id, so it reports nothing new for the same redirect count.
    run_fetch(request);
    EXPECT_EQ(events->names.size(), 2u);

    // Another request gets an id of its own.
    auto other_request = harness.create_request_for("http://localhost/"sv);
    run_fetch(other_request);
    EXPECT_EQ(events->names, (Vector<String> { "network.beforeRequestSent"_string, "network.fetchError"_string, "network.beforeRequestSent"_string, "network.fetchError"_string }));
    VERIFY(events->request_ids.size() == 4);
    EXPECT_EQ(events->request_ids[3], events->request_ids[2]);
    EXPECT_NE(events->request_ids[2], events->request_ids[0]);
}

TEST_CASE(a_request_that_starts_after_its_fetch_was_stopped_reports_the_stop)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto events = make_ref_counted<ReachedSteps>();
    WebDriverBiDiSession session {
        { "network.beforeRequestSent"_string, "network.fetchError"_string },
        [events](Web::HTML::Navigable const&, String const& name, JsonValue params) {
            auto const& object = params.as_object();
            auto event = ByteString::formatted("{} {}", name, object.get_u32("redirectCount"sv).value());
            if (auto error_text = object.get_string("errorText"sv); error_text.has_value())
                event = ByteString::formatted("{}: {}", event, *error_text);
            events->steps.append(move(event));
        },
    };

    auto request = harness.create_request_for("http://localhost/"sv);
    auto controller = harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard });
    controller->stop_fetch();
    // A redirect the stopped fetch follows moves its request on to the next redirect count.
    request->set_redirect_count(1);
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(request), [](auto& request) { return request->done(); }));

    EXPECT_EQ(events->steps, (Vector<ByteString> {
                                 "network.beforeRequestSent 0",
                                 "network.fetchError 0: Fetch was stopped",
                                 "network.beforeRequestSent 1",
                                 "network.fetchError 1: Fetch was stopped",
                             }));
}

namespace {

// A page client that records the network activity reported to it, and opens the files its page asks for.
class NetworkActivityPageClient final : public Web::PageClient {
    GC_CELL(NetworkActivityPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(NetworkActivityPageClient);

public:
    static GC::Ref<NetworkActivityPageClient> create(GC::Heap& heap, Web::PageId id)
    {
        auto client = heap.allocate<NetworkActivityPageClient>(id);
        client->m_page = Web::Page::create(client);
        return client;
    }

    virtual Web::PageId id() const override { return m_id; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { VERIFY_NOT_REACHED(); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 2, s_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual bool is_headless() const override { return true; }

    virtual void request_file(Web::FileRequest request) override
    {
        auto file = Core::File::open(request.path(), Core::File::OpenMode::Read);
        if (file.is_error()) {
            request.on_file_request_finish(file.release_error());
            return;
        }
        request.on_file_request_finish(file.value()->leak_fd());
    }

    virtual void page_did_start_network_request(u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& request_headers, ReadonlyBytes request_body, Optional<String> initiator_type, String const& referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::Request::Priority priority) override
    {
        VERIFY(request_headers.size() == 1);
        calls.append(ByteString::formatted("start {} {} {} {}: {} body {} initiator {} referrer policy {} navigation {} priority {}", request_id, method, url, request_headers[0].name, request_headers[0].value, StringView { request_body }, initiator_type.value_or("none"_string), referrer_policy, is_navigation_request, to_underlying(priority)));
    }

    virtual void page_did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> const& response_headers, Requests::CacheState cache_state) override
    {
        VERIFY(response_headers.size() == 1);
        calls.append(ByteString::formatted("headers {} {} {} {}: {} cache state {}", request_id, status_code, reason_phrase.value_or("none"_string), response_headers[0].name, response_headers[0].value, to_underlying(cache_state)));
    }

    virtual void page_did_receive_network_response_body(u64 request_id, ReadonlyBytes data) override
    {
        calls.append(ByteString::formatted("body {} {}", request_id, StringView { data }));
    }

    virtual void page_did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error) override
    {
        auto error = network_error.has_value() ? Requests::network_error_to_string(*network_error) : "none"sv;
        calls.append(ByteString::formatted("finish {} {} encoded {} error {}", request_id, body_size, timing_info.encoded_body_size, error));
    }

    Vector<ByteString> calls;

private:
    explicit NetworkActivityPageClient(Web::PageId id)
        : m_id(id)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    Web::PageId m_id;
    static inline u64 s_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(NetworkActivityPageClient);

}

// The active document of a new top-level traversable of the page.
static GC::Ref<Web::DOM::Document> create_document_of_page(Web::Page& page)
{
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page.set_top_level_traversable(traversable);
    return *traversable->active_document();
}

// One of each network activity, of the RequestServer request with the id.
static Vector<NetworkActivity> each_network_activity(u64 request_id)
{
    Vector<NetworkActivity> activities;
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = NetworkActivity::RequestStarted {
            .url = URL::Parser::basic_parse("https://example.com/upload"sv).release_value(),
            .method = "POST",
            .headers = { { "Accept"sv, "*/*"sv } },
            .body = Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("hello"sv.bytes()))),
            .initiator_type = "fetch"_string,
            .referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::NoReferrer,
            .is_navigation_request = false,
            .priority = Web::Fetch::Infrastructure::RequestPriority::High,
        },
    });
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = NetworkActivity::ResponseHeaders {
            .status_code = 200,
            .reason_phrase = "OK"_string,
            .headers = { { "Content-Type"sv, "text/plain"sv } },
            .cache_state = Requests::CacheState::Local,
        },
    });
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = NetworkActivity::ResponseBodyBytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("world"sv.bytes()))) },
    });
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = NetworkActivity::RequestFinished {
            .total_size = 5,
            .timing_info = { .encoded_body_size = 5 },
            .network_error = Requests::NetworkError::IncompleteContent,
        },
    });
    return activities;
}

TEST_CASE(network_activity_is_reported_to_the_page_of_the_realm_that_started_the_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto first_page_client = NetworkActivityPageClient::create(heap, Web::PageId { 1 });
    auto second_page_client = NetworkActivityPageClient::create(heap, Web::PageId { 2 });
    auto first_document = create_document_of_page(first_page_client->page());
    auto second_document = create_document_of_page(second_page_client->page());

    auto first_request = harness.create_request();
    auto first_controller = harness.fetch(first_request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard }, first_document);
    auto second_request = harness.create_request();
    auto second_controller = harness.fetch(second_request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard }, second_document);

    FetchEventSink first_event_sink { first_controller };
    for (auto& activity : each_network_activity(7))
        first_event_sink.handle_event(move(activity));
    FetchEventSink { second_controller }.handle_event(each_network_activity(8).take_first());

    EXPECT_EQ(first_page_client->calls, (Vector<ByteString> {
                                            "start 7 POST https://example.com/upload Accept: */* body hello initiator fetch referrer policy no-referrer navigation false priority 0",
                                            "headers 7 200 OK Content-Type: text/plain cache state 1",
                                            "body 7 world",
                                            "finish 7 5 encoded 5 error Connection was closed before entire response was received",
                                        }));
    EXPECT_EQ(second_page_client->calls, (Vector<ByteString> { "start 8 POST https://example.com/upload Accept: */* body hello initiator fetch referrer policy no-referrer navigation false priority 0" }));

    harness.run_tasks_until(GC::create_function(heap, GC::captures(first_request, second_request), [](auto& first_request, auto& second_request) {
        return first_request->done() && second_request->done();
    }));
}

TEST_CASE(loads_that_start_no_network_request_report_no_network_activity)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    ResourceLoaderWithoutRequestServer resource_loader { harness.realm->heap() };

    auto& heap = harness.realm->heap();
    auto page_client = NetworkActivityPageClient::create(heap, Web::PageId { 1 });
    auto document = create_document_of_page(page_client->page());

    // Each load reaches the loader and fails there: one finds no RequestServer, the other no resource.
    auto record = make_ref_counted<AlgorithmCalls>();
    for (auto url : { "http://127.0.0.1:8000/"sv, "resource://ladybird/no-such-resource"sv }) {
        FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard };
        input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
            auto const& message = response->network_error_message();
            record->calls.append(message.has_value() && message->starts_with_bytes("Load failed: "sv) ? "failed in the loader"sv : "other"sv);
        });
        auto request = harness.create_request_for(url);
        request->set_destination(Request::Destination::Image);
        (void)harness.fetch(request, move(input), document);
    }

    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 2; }));
    EXPECT_EQ(record->calls, (Vector<StringView> { "failed in the loader"sv, "failed in the loader"sv }));
    EXPECT(page_client->calls.is_empty());
}

TEST_CASE(an_agent_has_a_devtools_client_while_any_page_of_it_has_one)
{
    auto observers = Web::Fetch::Engine::NetworkActivityObservers::create();
    EXPECT(!observers->has_devtools_client());

    observers->page_did_gain_devtools_client();
    observers->page_did_gain_devtools_client();
    observers->page_did_lose_devtools_client();
    EXPECT(observers->has_devtools_client());

    observers->page_did_lose_devtools_client();
    EXPECT(!observers->has_devtools_client());
}

TEST_CASE(a_fetch_reads_the_network_activity_observers_of_its_agent)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto request = harness.create_request();
    auto controller = harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard });
    auto fetch_params = controller->original_fetch_params();
    VERIFY(fetch_params);
    harness.run_tasks_until(GC::create_function(heap, GC::captures(request), [](auto& request) { return request->done(); }));

    auto& agent = Web::HTML::surrounding_agent();
    VERIFY(fetch_params->engine_record());
    EXPECT_EQ(&fetch_params->engine_record()->network_activity_observers(), agent.network_activity_observers.ptr());

    agent.network_activity_observers->page_did_gain_devtools_client();
    EXPECT(fetch_params->engine_record()->network_activity_observers().has_devtools_client());
    agent.network_activity_observers->page_did_lose_devtools_client();
    EXPECT(!fetch_params->engine_record()->network_activity_observers().has_devtools_client());
}

static void expect_network_phases_of(Web::Fetch::Infrastructure::FetchTimingInfo const& timing_info, Requests::RequestTimingInfo const& finished)
{
    auto expected = timing_info.final_network_phases(finished, Web::HTML::CanUseCrossOriginIsolatedAPIs::No);
    EXPECT(timing_info.final_connection_timing_info().has_value());
    if (!timing_info.final_connection_timing_info().has_value())
        return;
    EXPECT_EQ(timing_info.final_connection_timing_info()->domain_lookup_start_time, expected.final_connection_timing_info.domain_lookup_start_time);
    EXPECT_EQ(timing_info.final_connection_timing_info()->connection_end_time, expected.final_connection_timing_info.connection_end_time);
    EXPECT_EQ(timing_info.final_network_request_start_time(), expected.final_network_request_start_time);
    EXPECT_EQ(timing_info.final_network_response_start_time(), expected.final_network_response_start_time);
}

static Web::Fetch::Engine::FetchNetworkPhases network_phases(double request_start_time)
{
    Web::Fetch::Infrastructure::ConnectionTimingInfo connection_timing_info;
    connection_timing_info.connection_start_time = request_start_time - 1;
    return {
        .final_connection_timing_info = move(connection_timing_info),
        .final_network_request_start_time = request_start_time,
        .final_network_response_start_time = request_start_time + 1,
    };
}

TEST_CASE(a_load_that_finishes_before_its_response_is_handed_over_gives_the_agent_its_network_phases)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "http://127.0.0.1:8000/finished-before-handover"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_finished(request_id, finished_timing_info(1000));
    run_until_responses(harness, record, 1);

    auto agent_timing_info = controller->control_block().agent_timing_info();
    VERIFY(agent_timing_info);
    expect_network_phases_of(*agent_timing_info, finished_timing_info(1000));
}

TEST_CASE(a_load_that_finishes_after_its_response_is_handed_over_gives_the_agent_its_network_phases)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto delivered = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/finished-after-handover"sv);
    auto agent_timing_info = delivered.controller->control_block().agent_timing_info();
    VERIFY(agent_timing_info);
    EXPECT(!agent_timing_info->final_connection_timing_info().has_value());

    request_server.send_finished(delivered.request_id, finished_timing_info(1000));
    run_until_encoded_size(harness, delivered.response, 1234);
    expect_network_phases_of(*agent_timing_info, finished_timing_info(1000));
    EXPECT_EQ(delivered.controller->control_block().agent_timing_info().ptr(), agent_timing_info.ptr());
}

TEST_CASE(network_phases_heard_before_a_handover_reach_each_timing_info_it_makes)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto controller = FetchController::create(Web::HTML::surrounding_agent().fetch_controller_registry);
    FetchEventSink event_sink { controller };
    auto& control_block = controller->control_block();

    event_sink.handle_event(RequestTransmissionEnded { network_phases(10) });
    EXPECT(!control_block.agent_timing_info());

    Web::Fetch::Engine::FetchTimingSeed seed;
    seed.post_redirect_start_time = 5;
    auto first = control_block.hand_over_timing(seed);
    EXPECT_EQ(first->post_redirect_start_time(), 5);
    EXPECT_EQ(first->final_network_request_start_time(), 10);

    // The next response of a manual redirect starts from the phases settled last. Phases settled later reach only the
    // newest handover's timing info.
    auto next = control_block.hand_over_timing(seed);
    EXPECT_NE(next.ptr(), first.ptr());
    EXPECT_EQ(next->final_network_request_start_time(), 10);
    event_sink.handle_event(RequestTransmissionEnded { network_phases(20) });
    EXPECT_EQ(next->final_network_request_start_time(), 20);
    EXPECT_EQ(first->final_network_request_start_time(), 10);
}

TEST_CASE(a_handover_gives_its_timing_info_each_field_of_the_seed_and_the_end_time_last_set_for_the_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto controller = FetchController::create(Web::HTML::surrounding_agent().fetch_controller_registry);
    auto& control_block = controller->control_block();

    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    timing_info->set_start_time(1);
    timing_info->set_redirect_start_time(2);
    timing_info->set_redirect_end_time(3);
    timing_info->set_post_redirect_start_time(4);
    timing_info->set_final_service_worker_start_time(5);
    timing_info->set_server_timing_headers({ "total;dur=6"_string });
    timing_info->set_render_blocking(true);
    control_block.set_end_time(7);

    auto handed_over = control_block.hand_over_timing(timing_info->seed());
    EXPECT_EQ(handed_over->start_time(), 1);
    EXPECT_EQ(handed_over->redirect_start_time(), 2);
    EXPECT_EQ(handed_over->redirect_end_time(), 3);
    EXPECT_EQ(handed_over->post_redirect_start_time(), 4);
    EXPECT_EQ(handed_over->final_service_worker_start_time(), 5);
    EXPECT_EQ(handed_over->server_timing_headers(), (Vector<String> { "total;dur=6"_string }));
    EXPECT(handed_over->render_blocking());
    EXPECT_EQ(handed_over->end_time(), 7);

    // An end time set later reaches it too.
    control_block.set_end_time(8);
    EXPECT_EQ(handed_over->end_time(), 8);
}

namespace {

// What the steps awaiting a body's end saw when they ran.
struct EndOfBodySteps : RefCounted<EndOfBodySteps> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<double> end_times;
    bool had_network_phases { false };
};

}

TEST_CASE(the_network_phases_of_a_load_reach_the_agent_before_the_end_of_its_body)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto controller = FetchController::create(Web::HTML::surrounding_agent().fetch_controller_registry);
    FetchEventSink event_sink { controller };
    auto agent_timing_info = controller->control_block().hand_over_timing({});
    auto response = Response::create();

    auto steps = make_ref_counted<EndOfBodySteps>();
    controller->when_body_ends(Web::Fetch::Engine::DeliveryId(1), response, GC::create_function(harness.realm->heap(), GC::captures(steps, agent_timing_info), [](auto& steps, auto& agent_timing_info, double end_time) {
        steps->end_times.append(end_time);
        steps->had_network_phases = agent_timing_info->final_connection_timing_info().has_value();
    }));

    event_sink.handle_event(RequestTransmissionEnded { network_phases(10) });
    EXPECT(steps->end_times.is_empty());
    EXPECT_EQ(agent_timing_info->final_network_request_start_time(), 10);

    EndOfBody end_of_body {
        .delivery_id = Web::Fetch::Engine::DeliveryId(1),
        .final_sizes = { .encoded_body_size = 3, .decoded_body_size = 4 },
        .end_time = 30,
    };
    event_sink.handle_event(move(end_of_body));
    EXPECT_EQ(steps->end_times, (Vector<double> { 30 }));
    EXPECT(steps->had_network_phases);
    EXPECT_EQ(response->body_info().encoded_size, 3u);
    EXPECT_EQ(response->body_info().decoded_size, 4u);
}

namespace {

// The WebDriver BiDi responseCompleted events that reached the event sink, by redirect count.
struct CompletedResponses : RefCounted<CompletedResponses> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<u32> redirect_counts;
    Vector<double> request_starts;
    Vector<double> response_ends;
};

}

TEST_CASE(a_redirected_fetch_reports_the_end_time_of_its_final_body_to_webdriver_bidi_and_resource_timing)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto completed = make_ref_counted<CompletedResponses>();
    WebDriverBiDiSession session {
        { "network.responseCompleted"_string },
        [completed](Web::HTML::Navigable const&, String const&, JsonValue params) {
            auto const& object = params.as_object();
            completed->redirect_counts.append(object.get_u32("redirectCount"sv).value());
            auto timings = object.get_object("request"sv)->get_object("timings"sv);
            completed->request_starts.append(timings->get_double_with_precision_loss("requestStart"sv).value());
            completed->response_ends.append(timings->get_double_with_precision_loss("responseEnd"sv).value());
        },
    };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/redirect"sv);
    request->set_initiator_type(Request::InitiatorType::Fetch);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto& control_block = controller->control_block();

    // The redirect completes, and WebDriver BiDi sets the fetch's end time.
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { { "Location"sv, "/final"sv } });
    request_server.send_finished(redirect_id, finished_timing_info(1000));
    auto final_id = request_server.wait_for_request("GET"sv);
    auto redirect_end_time = control_block.end_time();
    EXPECT_NE(redirect_end_time, 0);

    // The final body ends once the clock is past the redirect's end time by more than a coarsening interval.
    request_server.send_headers(final_id, 200);
    run_until_responses(harness, record, 1);
    auto clock_tick = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [] { });
    clock_tick->start();
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(redirect_end_time), [](auto& redirect_end_time) {
        return Web::HighResolutionTime::unsafe_shared_current_time() > redirect_end_time + 1;
    }));
    clock_tick->stop();
    request_server.send_finished(final_id, finished_timing_info(2000));
    auto response = record->responses[0]->record()->unsafe_response();
    run_until_encoded_size(harness, response, 1234);

    // The timing report and WebDriver BiDi share the fetch's end time, which is the final body's.
    auto agent_timing_info = control_block.agent_timing_info();
    VERIFY(agent_timing_info);
    EXPECT(agent_timing_info->end_time() > redirect_end_time + 1);
    EXPECT_EQ(control_block.end_time(), agent_timing_info->end_time());
    EXPECT_EQ(completed->redirect_counts, (Vector<u32> { 0, 1 }));
    VERIFY(completed->response_ends.size() == 2);
    auto time_origin = request->client_snapshot()->time_origin;
    EXPECT_EQ(completed->response_ends[0], Web::HighResolutionTime::coarsen_time(redirect_end_time) - time_origin);
    EXPECT_EQ(completed->response_ends[1], Web::HighResolutionTime::coarsen_time(agent_timing_info->end_time()) - time_origin);
    EXPECT_NE(completed->response_ends[1], completed->response_ends[0]);

    // The final body's completion reports the network phases of the final load.
    EXPECT_EQ(completed->request_starts[1], Web::HighResolutionTime::coarsen_time(agent_timing_info->final_network_request_start_time()) - time_origin);
}

TEST_CASE(the_next_response_of_a_manual_redirect_takes_the_end_time_last_set_for_the_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };
    WebDriverBiDiSession session { { "network.responseCompleted"_string }, [](Web::HTML::Navigable const&, String const&, JsonValue) { } };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/manual-redirect"sv);
    request->set_mode(Request::Mode::Navigate);
    request->set_destination(Request::Destination::Document);
    request->set_redirect_mode(Request::RedirectMode::Manual);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto& control_block = controller->control_block();

    // WebDriver BiDi completes the redirect, which sets the fetch's end time before the redirect is handed over.
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { { "Location"sv, "/final"sv } });
    run_until_responses(harness, record, 1);
    auto redirect_end_time = control_block.end_time();
    EXPECT_NE(redirect_end_time, 0);
    auto redirect_timing_info = control_block.agent_timing_info();
    VERIFY(redirect_timing_info);
    EXPECT_EQ(redirect_timing_info->end_time(), redirect_end_time);

    {
        Web::HTML::TemporaryExecutionContext execution_context { *harness.realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        controller->process_next_manual_redirect();
    }
    auto final_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(final_id, 200);
    request_server.send_finished(final_id, finished_timing_info(2000));
    run_until_responses(harness, record, 2);
    run_until_encoded_size(harness, record->responses[1]->record()->unsafe_response(), 1234);

    // The final response's timing info takes the fetch's end time, and the redirect and network fields of its own hop.
    auto final_timing_info = control_block.agent_timing_info();
    VERIFY(final_timing_info);
    EXPECT_NE(final_timing_info.ptr(), redirect_timing_info.ptr());
    EXPECT_EQ(final_timing_info->end_time(), redirect_end_time);
    auto fetch_params = controller->original_fetch_params();
    VERIFY(fetch_params);
    auto const& engine_timing_info = *fetch_params->timing_info();
    EXPECT_NE(engine_timing_info.redirect_end_time(), redirect_timing_info->redirect_end_time());
    EXPECT_EQ(final_timing_info->redirect_start_time(), engine_timing_info.redirect_start_time());
    EXPECT_EQ(final_timing_info->redirect_end_time(), engine_timing_info.redirect_end_time());
    EXPECT_EQ(final_timing_info->post_redirect_start_time(), engine_timing_info.post_redirect_start_time());
    expect_network_phases_of(*final_timing_info, finished_timing_info(2000));
}

TEST_CASE(a_cors_preflight_reports_none_of_its_network_phases_to_its_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preflighted"sv);
    request->set_method("PUT");
    request->set_mode(Request::Mode::CORS);
    request->set_unsafe_request(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));

    // The preflight's request ends right after its headers.
    auto preflight_id = request_server.wait_for_request("OPTIONS"sv);
    request_server.send_headers(preflight_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv }, { "Access-Control-Allow-Methods"sv, "PUT"sv } });
    request_server.send_finished(preflight_id, finished_timing_info(9000));
    auto request_id = request_server.wait_for_request("PUT"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    run_until_responses(harness, record, 1);

    auto agent_timing_info = controller->control_block().agent_timing_info();
    VERIFY(agent_timing_info);
    EXPECT(!agent_timing_info->final_connection_timing_info().has_value());

    request_server.send_finished(request_id, finished_timing_info(1000));
    run_until_encoded_size(harness, record->responses[0]->record()->unsafe_response(), 1234);
    expect_network_phases_of(*agent_timing_info, finished_timing_info(1000));
}

TEST_CASE(a_file_navigation_keeps_the_network_phases_of_its_load)
{
    auto const path = ByteString::formatted("{}/TestFetchAlgorithms-{:016x}.html", Core::StandardPaths::tempfile_directory(), get_random<u64>());
    {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew));
        MUST(file->write_until_depleted("<p>file</p>"sv.bytes()));
    }
    ScopeGuard remove_file = [&path] { MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed)); };

    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    ResourceLoaderWithoutRequestServer resource_loader { harness.realm->heap() };
    auto page_client = NetworkActivityPageClient::create(harness.realm->heap(), Web::PageId { 1 });
    auto document = create_document_of_page(page_client->page());

    auto record = make_ref_counted<AlgorithmCalls>();
    auto url = URL::create_with_file_scheme(path);
    VERIFY(url.has_value());
    auto request = harness.create_request_for(url->serialize());
    // A traversal back to a file: entry, which fetches it with the client of the document it replaces.
    request->set_mode(Request::Mode::Navigate);
    request->set_destination(Request::Destination::Document);
    request->set_redirect_mode(Request::RedirectMode::Manual);
    request->set_history_navigation(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record), document));
    run_until_responses(harness, record, 1);
    auto response = record->responses[0]->record()->unsafe_response();
    run_until_encoded_size(harness, response, 11);

    // A load without a RequestServer request has its network phases at its post-redirect start time.
    auto full_timing_info = controller->full_timing_info();
    VERIFY(full_timing_info);
    EXPECT_EQ(full_timing_info.ptr(), controller->control_block().agent_timing_info().ptr());
    expect_network_phases_of(*full_timing_info, {});
}

TEST_CASE(algorithms_that_consume_a_body_need_a_consuming_intent)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto algorithms_that_consume_a_body = [&heap](Web::Fetch::Engine::BodyIntent body_intent) {
        FetchAlgorithms::Input input { body_intent };
        input.process_response_consume_body = GC::create_function(heap, [](GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { });
        return FetchAlgorithms::create(move(input));
    };

    EXPECT_DEATH("algorithms that consume a body have a consuming intent", (void)algorithms_that_consume_a_body(Web::Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent));
    EXPECT_DEATH("algorithms that consume a body have a consuming intent", (void)algorithms_that_consume_a_body(Web::Fetch::Engine::BodyIntent::DrainAndDiscard));
    EXPECT_EQ(algorithms_that_consume_a_body(Web::Fetch::Engine::BodyIntent::SyncAccumulate)->body_intent(), Web::Fetch::Engine::BodyIntent::SyncAccumulate);
}
