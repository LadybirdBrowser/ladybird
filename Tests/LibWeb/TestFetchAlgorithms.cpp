/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <AK/JsonObject.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibJS/Runtime/Error.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/EngineEvent.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
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
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
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
using Web::Fetch::Engine::WholeBodyTransport;
using Web::Fetch::Engine::Event::NetworkActivity;
using Web::Fetch::Engine::Event::ReadRequestBodyStream;
using Web::Fetch::Engine::Event::RequestBodyEndOfBody;
using Web::Fetch::Engine::Event::RequestBodyProgress;
using Web::Fetch::Engine::Event::RequestBodySentFromSource;
using Web::Fetch::Fetching::FetchEventSink;
using Web::Fetch::Fetching::NetworkBodyPump;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchParams;
using Web::Fetch::Infrastructure::Request;
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

// Records each stop that reaches the fetch engine.
class StopRecordingEngine final : public EngineCommandSink {
public:
    explicit StopRecordingEngine(NonnullRefPtr<ReachedSteps> reached)
        : m_reached(move(reached))
    {
    }

    virtual void handle_command(EngineCommand command) override
    {
        command.visit(
            [&](Web::Fetch::Engine::Command::Abort const& abort) { m_reached->steps.append(ByteString::formatted("abort {}", abort.fetch_id.value())); },
            [&](Web::Fetch::Engine::Command::Terminate const& terminate) { m_reached->steps.append(ByteString::formatted("terminate {}", terminate.fetch_id.value())); },
            [&](Web::Fetch::Engine::Command::Stop const& stop) { m_reached->steps.append(ByteString::formatted("stop {}", stop.fetch_id.value())); },
            [](auto const&) { FAIL("unexpected command"); });
    }

private:
    NonnullRefPtr<ReachedSteps> m_reached;
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
    auto fetch_executor = Web::Fetch::Engine::InlineFetchExecutor::create(move(services), make<StopRecordingEngine>(reached));

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

// The WebDriver BiDi network events that reached the event sink, by name and request id.
struct ReportedNetworkEvents : RefCounted<ReportedNetworkEvents> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<String> names;
    Vector<String> request_ids;
};

}

TEST_CASE(a_request_fetched_again_keeps_its_webdriver_bidi_request_id)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto events = make_ref_counted<ReportedNetworkEvents>();
    Web::WebDriver::BiDi::set_event_sink({
        .on_event = [events](Web::HTML::Navigable const&, String const& name, JsonValue params) {
            events->names.append(name);
            events->request_ids.append(*params.as_object().get_object("request"sv)->get_string("request"sv));
        },
        .on_navigation_event = {},
    });
    Web::WebDriver::BiDiSessionState session_state;
    session_state.is_active = true;
    session_state.enabled_events = { "network.beforeRequestSent"_string, "network.fetchError"_string };
    Web::WebDriver::BiDi::set_session_state(move(session_state));
    ScopeGuard end_session = [] {
        Web::WebDriver::BiDi::set_session_state({});
        Web::WebDriver::BiDi::set_event_sink({});
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
    Web::WebDriver::BiDi::set_event_sink({
        .on_event = [events](Web::HTML::Navigable const&, String const& name, JsonValue params) {
            auto const& object = params.as_object();
            auto event = ByteString::formatted("{} {}", name, object.get_u32("redirectCount"sv).value());
            if (auto error_text = object.get_string("errorText"sv); error_text.has_value())
                event = ByteString::formatted("{}: {}", event, *error_text);
            events->steps.append(move(event));
        },
        .on_navigation_event = {},
    });
    Web::WebDriver::BiDiSessionState session_state;
    session_state.is_active = true;
    session_state.enabled_events = { "network.beforeRequestSent"_string, "network.fetchError"_string };
    Web::WebDriver::BiDi::set_session_state(move(session_state));
    ScopeGuard end_session = [] {
        Web::WebDriver::BiDi::set_session_state({});
        Web::WebDriver::BiDi::set_event_sink({});
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

// A page client that records the network activity reported to it.
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
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }

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
