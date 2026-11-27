/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestFixtures.h"
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
#include <LibGC/HeapAccess.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Script.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
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
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
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
#include <LibWeb/HTML/PreloadEntry.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/Network.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWebCommon/WebDriver/Network.h>

using Web::Fetch::Engine::FetchBodyDeliveryGate;
using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::NetworkTransportHandle;
using Web::Fetch::Engine::WholeBodyTransport;
using Web::Fetch::Engine::Command::TransportControl;
using Web::Fetch::Engine::Event::EndOfBody;
using Web::Fetch::Engine::Event::ReadRequestBodyStream;
using Web::Fetch::Engine::Event::RequestBodyEndOfBody;
using Web::Fetch::Engine::Event::RequestBodyProgress;
using Web::Fetch::Engine::Event::RequestBodySentFromSource;
using Web::Fetch::Engine::Event::RequestTransmissionEnded;
using Web::Fetch::Engine::Event::TransportActivity;
using Web::Fetch::Fetching::FetchEventSink;
using Web::Fetch::Fetching::NetworkBodyPump;
using Web::Fetch::Fetching::PendingResponse;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchController;
using Web::Fetch::Infrastructure::FetchParams;
using Web::Fetch::Infrastructure::Request;
using Web::Fetch::Infrastructure::Response;
using Web::Fetch::Infrastructure::ResponseView;

using namespace FetchTest;

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

namespace {

class IgnoringProducerSink final : public Web::Fetch::Engine::FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
};

}

static FetchAlgorithms::Input algorithms_recording_their_calls(GC::Heap& heap, NonnullRefPtr<AlgorithmCalls> const& record)
{
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView> response) {
        record->calls.append("process response"sv);
        record->responses.append(GC::make_root(response));
    });
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) {
        record->calls.append("process response end-of-body"sv);
    });
    input.process_response_consume_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes body) {
        record->calls.append("process response consume body"sv);
        if (auto const* bytes = body.get_pointer<Core::ImmutableBytes>())
            record->consumed_bytes = MUST(ByteBuffer::copy(bytes->bytes()));
    });
    return input;
}

// Delivers the response with the body to a fetch of the harness's document, as the fetch engine hands a response over,
// and returns what the fetch's algorithms saw once each has run.
static NonnullRefPtr<AlgorithmCalls> deliver_to_a_fetch(FetchHarness const& harness, NonnullRefPtr<Response> response, Web::Fetch::Engine::LocalBodyPresentation body)
{
    auto& heap = harness.realm->heap();
    auto& agent = Web::HTML::surrounding_agent();
    auto record = make_ref_counted<AlgorithmCalls>();
    auto controller = GC::make_root(FetchController::create(agent.fetch_controller_registry));
    auto request = harness.create_request_for("https://example.com/delivered"sv);
    auto algorithms = FetchAlgorithms::create(algorithms_recording_their_calls(heap, record));
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    auto fetch_params = controller->create_fetch_params(move(request), algorithms, move(timing_info), Web::current_loader_config());
    controller->set_realm(*harness.realm);
    controller->set_task_destination(GC::Ref { harness.realm->global_object() });
    Web::Fetch::Engine::FetchId fetch_id { 1 };
    controller->set_started_fetch(fetch_id, *agent.fetch_executor);

    Web::Fetch::Engine::FetchEngineRecords records { make<Web::Fetch::Engine::FetchEngineRecords::Agent>() };
    auto event_sink = make<FetchEventSink>(*controller);
    auto engine_record = records.start(fetch_id, move(event_sink), Web::Fetch::Engine::NetworkActivityObservers::create());
    Web::Fetch::Engine::Event::ResponseDelivery delivery {
        .response = move(response),
        .body = move(body),
        .timing_seed = {},
        .redirect_continuation_id = {},
    };
    engine_record->main_fetch_hands_over(move(delivery));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->calls.size() == 3; }));
    return record;
}

TEST_CASE(a_delivered_response_runs_the_algorithms_in_order_over_the_body_it_was_delivered_with)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    using Web::Fetch::Engine::Delivered;
    using Web::Fetch::Engine::NonNullLocalBodyHandle;
    auto expected_calls = Vector<StringView> { "process response"sv, "process response end-of-body"sv, "process response consume body"sv };

    // A body of bytes, which stays in the response.
    auto bytes = MUST(Core::ImmutableBytes::copy("bytes"sv.bytes()));
    auto response_with_bytes = Response::create();
    response_with_bytes->set_body(Web::Fetch::Infrastructure::ResponseBody::from_bytes(bytes));
    auto over_bytes = deliver_to_a_fetch(harness, response_with_bytes, Delivered<NonNullLocalBodyHandle> { bytes });
    EXPECT_EQ(over_bytes->calls, expected_calls);
    VERIFY(over_bytes->responses.size() == 1);
    EXPECT_EQ(over_bytes->responses[0]->record().ptr(), response_with_bytes.ptr());
    VERIFY(over_bytes->consumed_bytes.has_value());
    EXPECT_EQ(*over_bytes->consumed_bytes, MUST(ByteBuffer::copy("bytes"sv.bytes())));

    // A body in a channel, which the response no longer holds.
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink));
    EXPECT_EQ(channel->write(MUST(Core::ImmutableBytes::copy("channel"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)channel->close();
    auto response_with_channel = Response::create();
    response_with_channel->set_body(Web::Fetch::Infrastructure::ResponseBody { Web::Fetch::Infrastructure::ResponseBody::ReadThroughStream {}, 7 });
    auto over_channel = deliver_to_a_fetch(harness, response_with_channel, Delivered<NonNullLocalBodyHandle> { channel });
    EXPECT_EQ(over_channel->calls, expected_calls);
    VERIFY(over_channel->responses.size() == 1);
    EXPECT_EQ(over_channel->responses[0]->record().ptr(), response_with_channel.ptr());
    VERIFY(over_channel->consumed_bytes.has_value());
    EXPECT_EQ(*over_channel->consumed_bytes, MUST(ByteBuffer::copy("channel"sv.bytes())));
}

// The fetches of a data: URL and of a network error have no live load at their handover, and end right after it.
TEST_CASE(a_fetch_with_no_live_load_at_its_handover_ends_after_its_response_reaches_the_agent)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    auto run_until_responses = [&](size_t count) {
        harness.run_tasks_until(GC::create_function(heap, GC::captures(record, count), [](auto& record, auto& count) { return record->responses.size() == count; }));
    };

    auto data_request = harness.create_request_for("data:text/plain,hello"sv);
    (void)harness.fetch(data_request, recording_responses(heap, record));
    harness.run_until_step(commands, "end of fetch 1"sv);
    run_until_responses(1);

    auto network_error_request = harness.create_request();
    (void)harness.fetch(network_error_request, recording_responses(heap, record));
    harness.run_until_step(commands, "end of fetch 2"sv);
    run_until_responses(2);

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of fetch 1", "start fetch 2", "end of fetch 2" }));
    VERIFY(record->responses.size() == 2);
    EXPECT(!record->responses[0]->is_network_error());
    EXPECT(record->responses[1]->is_network_error());
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

// A fetch that has run to its end, with a response body whose transport the fetch's engine record keeps.
struct FetchWithBody {
    GC::Root<FetchController> controller;
    NonnullRefPtr<FetchParams const> fetch_params;
    NonnullRefPtr<NetworkBodyPump> body_pump;
    NonnullRefPtr<WholeBodyTransport> transport;
};

}

static FetchWithBody fetch_with_a_body(FetchHarness const& harness)
{
    auto started = harness.record_started_fetches();
    auto request = harness.create_request();
    auto controller = GC::make_root(harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard }));
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(request), [](auto& request) { return request->done(); }));
    VERIFY(started->fetch_params.size() == 1);
    auto fetch_params = started->fetch_params.take_first();

    // The case's fetch has run to its end, so another engine's fetch starts the body's load.
    auto body_pump = NetworkBodyPump::create(fetch_params, FetchBodyDeliveryGate::NavigationHold::None, nullptr);
    auto transport = Web::Fetch::Fetching::create_whole_body_transport(*body_pump->delivery_gate(), *body_pump->channel(), start_a_load_of_another_engine());
    fetch_params->set_body_transport(*transport);
    return { move(controller), move(fetch_params), move(body_pump), move(transport) };
}

// Hands the transport its body, as the network fetch does once the response's headers arrive.
static void deliver_body(FetchWithBody& fetch, WholeBodyTransport::OnData on_data)
{
    fetch.transport->attach_to(*fetch.body_pump->delivery_gate());
    auto body = Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("hello"sv.bytes())));
    fetch.transport->set_body(move(body), move(on_data));
}

TEST_CASE(each_stop_of_a_started_fetch_reaches_the_engine_for_its_fetch_id)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto reached = make_ref_counted<ReachedSteps>();
    Web::Fetch::Engine::FetchExecutorServices services {
        .event_loop = Core::EventLoop::current_weak(),
        .known_hsts_host_provider = Web::HTML::surrounding_agent().fetch_executor->known_hsts_host_provider(),
    };
    auto fetch_executor = Web::Fetch::Engine::InlineFetchExecutor::create(move(services), make<CommandRecordingEngine>(reached));

    Web::Fetch::Infrastructure::FetchControllerRegistry registry;
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

    stopped->stop_fetch();
    {
        Web::HTML::TemporaryExecutionContext execution_context { *harness.realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        aborted->abort(*harness.realm, {});
        never_started->abort(*harness.realm, {});
    }
    terminated->terminate();
    never_started->terminate();
    harness.run_queued_invocations();

    EXPECT_EQ(reached->steps, (Vector<ByteString> { "stop 1", "abort 2", "terminate 3" }));
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
    harness.run_queued_invocations();

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
    harness.run_queued_invocations();

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

// The fetch params of a fetch that has run to its end, whose engine record defers work through the agent's executor.
static NonnullRefPtr<FetchParams const> fetch_params_of_a_finished_fetch(FetchHarness const& harness)
{
    auto started = harness.record_started_fetches();
    auto request = harness.create_request();
    auto controller = GC::make_root(harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard }));
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(request), [](auto& request) { return request->done(); }));
    VERIFY(started->fetch_params.size() == 1);
    auto fetch_params = started->fetch_params.take_first();
    VERIFY(fetch_params->engine_record());
    return fetch_params;
}

TEST_CASE(a_pending_response_lets_go_of_its_callback_once_the_callback_has_run)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto fetch_params = fetch_params_of_a_finished_fetch(harness);

    auto reached = make_ref_counted<ReachedSteps>();
    auto captured = make_ref_counted<ReachedSteps>();
    auto pending_response = PendingResponse::create(*fetch_params);
    pending_response->when_loaded([reached, captured](NonnullRefPtr<Response>) { reached->steps.append("callback"); });
    pending_response->resolve(Response::create());
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(reached), [](auto& reached) { return !reached->steps.is_empty(); }));

    EXPECT_EQ(reached->steps, (Vector<ByteString> { "callback" }));
    EXPECT_EQ(captured->ref_count(), 1u);
    EXPECT_EQ(pending_response->ref_count(), 1u);
}

TEST_CASE(a_pending_response_never_resolved_is_freed_with_its_last_holder)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto fetch_params = fetch_params_of_a_finished_fetch(harness);

    auto reached = make_ref_counted<ReachedSteps>();
    RefPtr pending_response = PendingResponse::create(*fetch_params);
    pending_response->when_loaded([reached](NonnullRefPtr<Response>) { reached->steps.append("callback"); });
    harness.settle();
    EXPECT_EQ(pending_response->ref_count(), 1u);

    pending_response = nullptr;
    EXPECT_EQ(reached->ref_count(), 1u);
    EXPECT(reached->steps.is_empty());
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
static FetchAlgorithms::RequestBodyReader create_request_body_reader(JS::Realm& realm, Function<void(Web::Streams::ReadableStream&)> set_up_stream, GC::Ptr<Web::Streams::PullAlgorithm> pull = {})
{
    Web::HTML::TemporaryExecutionContext execution_context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto stream = Web::Streams::ReadableStream::create(realm);
    stream->set_up_with_byte_reading_support(realm, pull);
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

TEST_CASE(an_event_after_the_end_of_its_fetch_is_dropped)
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
    event_sink.handle_event(Web::Fetch::Engine::Event::FetchEnded {});
    event_sink.handle_event(RequestBodyProgress { .length = 4 });
    event_sink.handle_event(RequestBodyEndOfBody {});

    // A task queued after the request body's tasks runs after them.
    Web::Fetch::Infrastructure::queue_fetch_task(controller, controller->task_destination(), GC::create_function(heap, GC::captures(record), [](auto& record) { record->later_task_ran = true; }));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return record->later_task_ran; }));

    EXPECT_EQ(record->calls, (Vector<ByteString> { "chunk 3" }));
}

// The end of a fetch drops the wait for the end of its body, and a wait from a handover after the fetch's end.
TEST_CASE(a_fetch_that_has_ended_awaits_the_end_of_no_body)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request();
    auto controller = harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard });
    auto steps = GC::create_function(heap, GC::captures(record), [](auto& record, double) { ++record->call_count; });
    controller->when_body_ends(Web::Fetch::Engine::DeliveryId(1), steps);
    FetchEventSink { controller }.handle_event(Web::Fetch::Engine::Event::FetchEnded {});
    controller->when_body_ends(Web::Fetch::Engine::DeliveryId(2), steps);

    for (u64 delivery_id : { 1, 2 })
        controller->did_end_body({ .delivery_id = Web::Fetch::Engine::DeliveryId(delivery_id), .end_time = 1 });
    EXPECT_EQ(record->call_count, 0u);
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

// The network step reads a request body from its stream, whose pull aborts the fetch and spins the event loop until
// the fetch has ended.
TEST_CASE(a_request_body_stream_that_aborts_its_fetch_while_the_network_step_reads_it_ends_the_fetch_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto& heap = harness.realm->heap();
    auto pull = GC::create_function(heap, GC::captures(harness.realm, commands), [](auto& realm, auto& commands) {
        find_controller(*token_of_fetch_for("/pulled"sv))->abort(*realm, {});
        run_tasks_until(realm->heap(), GC::create_function(realm->heap(), GC::captures(commands), [](auto& commands) { return commands->steps.contains_slow("end of fetch 1"sv); }));
        return Web::WebIDL::create_resolved_promise(*realm, JS::js_undefined());
    });
    auto record = make_ref_counted<AlgorithmCalls>();
    auto input = recording_responses(heap, record);
    input.request_body_reader = create_request_body_reader(*harness.realm, [](auto&) { }, pull);
    auto request = harness.create_request_for("http://127.0.0.1:8000/pulled"sv);
    request->set_method("POST"sv);
    request->set_body(Web::Fetch::Infrastructure::FetchBody {});
    auto controller = GC::make_root(harness.fetch(request, move(input)));

    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    // The network step sends nothing more once the fetch has ended inside it.
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 POST /pulled", "stop 0" }));
    EXPECT(record->responses.is_empty());
    EXPECT(!is_rooted(controller->token()));
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

// A navigation body adopted from a RequestServer that the test stands in for. The commands posted to the agent's
// executor are recorded, and reach a fetch engine only when the test applies them.
class AdoptedNavigationBody {
public:
    explicit AdoptedNavigationBody(FetchHarness const& harness)
        : m_harness(harness)
        , m_request_server(harness)
    {
        auto& agent = Web::HTML::surrounding_agent();
        Web::Fetch::Engine::FetchExecutorServices services {
            .event_loop = Core::EventLoop::current_weak(),
            .known_hsts_host_provider = agent.fetch_executor->known_hsts_host_provider(),
        };
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
        m_harness.run_queued_invocations();
        for (auto& command : m_posted_commands->commands)
            m_engine_executor->post(move(command));
        m_posted_commands->commands.clear();
        m_harness.run_queued_invocations();
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
    harness.run_queued_invocations();
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

    harness.run_queued_invocations();
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
    harness.run_until_responses(record, 1);
    auto response = record->responses[0]->record()->unsafe_response();
    auto delivery_id = response->network_transport()->body_channel->delivery_id();
    VERIFY(delivery_id.has_value());
    return { move(controller), move(response), *delivery_id, request_id };
}

// Reports the end of the delivered body as the engine would once the body had arrived whole.
static void report_end_of_body(DeliveredBody const& delivered)
{
    EndOfBody end_of_body {
        .delivery_id = delivered.delivery_id,
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
    harness.run_until_responses(record, 1);

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

    auto delivered = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/stopped"sv, Request::InitiatorType::Fetch);
    delivered.controller->stop_fetch();
    report_end_of_body(delivered);
    EXPECT_NE(delivered.controller->control_block().end_time(), 1);
}

TEST_CASE(a_body_that_fails_or_moves_away_reports_no_end_and_ends_its_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto failed = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/failed"sv, Request::InitiatorType::Fetch);
    auto transferred = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/transferred"sv, Request::InitiatorType::Fetch);
    EXPECT(is_rooted(failed.controller->token()));
    EXPECT(is_rooted(transferred.controller->token()));
    request_server.send_finished(failed.request_id, finished_timing_info(1000), Requests::NetworkError::IncompleteContent);
    request_server.send_transferred(transferred.request_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.run_until_step(commands, "end of fetch 2"sv);

    EXPECT(failed.response->network_transport()->body_channel->peek_up_to(0).state.has<FetchByteChannel::Errored>());
    EXPECT(transferred.response->network_transport()->body_channel->peek_up_to(0).state.has<FetchByteChannel::Transferred>());
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "start fetch 2", "end of fetch 1", "end of fetch 2" }));
    EXPECT(!is_rooted(failed.controller->token()));
    EXPECT(!is_rooted(transferred.controller->token()));

    // The end of each fetch drops the wait for the end of its body.
    report_end_of_body(failed);
    report_end_of_body(transferred);
    EXPECT_NE(failed.controller->control_block().end_time(), 1);
    EXPECT_NE(transferred.controller->control_block().end_time(), 1);
}

TEST_CASE(a_fetch_ends_once_its_response_is_handed_over_and_its_body_has_arrived)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    // The body arrives after the handover.
    auto delivered = deliver_a_body(harness, request_server, "http://127.0.0.1:8000/arrives-after-handover"sv);
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1" }));
    EXPECT(is_rooted(delivered.controller->token()));
    request_server.send_finished(delivered.request_id, finished_timing_info(1000));
    harness.run_until_step(commands, "end of fetch 1"sv);
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of body of 1", "end of fetch 1" }));
    EXPECT(!is_rooted(delivered.controller->token()));

    // The body arrives before the handover. The fetch is rooted from its start, before the engine runs it.
    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "http://127.0.0.1:8000/arrives-before-handover"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    EXPECT(is_rooted(controller->token()));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_finished(request_id, finished_timing_info(1000));
    harness.run_until_responses(record, 1);
    EXPECT(!record->responses[0]->is_network_error());
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of body of 1", "end of fetch 1", "start fetch 2", "end of fetch 2" }));
    EXPECT(!is_rooted(controller->token()));
}

// HTTP-network-or-cache fetch retries a request whose response is a 421 on a new connection, and nothing stops the
// first request.
TEST_CASE(a_fetch_retried_after_a_421_ends_once_the_load_of_its_first_request_has_ended)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/misdirected"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto first_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(first_id, 421);
    auto retry_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(retry_id, 200);
    harness.run_until_responses(record, 1);
    request_server.send_finished(retry_id);
    harness.run_until_step(commands, "end of body of 1"sv);
    harness.settle();

    // The retry's response is handed over and its body has arrived, but the first request's body still arrives.
    EXPECT_EQ(record->responses[0]->record()->unsafe_response()->status(), 200u);
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of body of 1" }));
    EXPECT(is_rooted(controller->token()));

    request_server.send_finished(first_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of body of 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /misdirected", "start 1 GET /misdirected" }));
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_fetch_whose_request_fails_before_its_response_ends_at_the_handover_of_its_network_error)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/fails"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_finished(request_id, {}, Requests::NetworkError::UnableToConnect);
    harness.run_until_responses(record, 1);

    EXPECT(record->responses[0]->is_network_error());
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of fetch 1" }));
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_cors_preflight_runs_under_the_controller_of_the_fetch_it_is_for)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto& registry = Web::HTML::surrounding_agent().fetch_controller_registry;
    auto controllers_before = registry.controllers();
    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preflighted"sv);
    request->set_mode(Request::Mode::CORS);
    request->set_use_cors_preflight(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto preflight_id = request_server.wait_for_request("OPTIONS"sv);
    for (auto const& other : registry.controllers()) {
        if (other.ptr() != controller.ptr())
            EXPECT(any_of(controllers_before, [&](auto const& before) { return before.ptr() == other.ptr(); }));
    }
    EXPECT(is_rooted(controller->token()));

    request_server.send_headers(preflight_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_finished(request_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 OPTIONS /preflighted", "stop 0", "start 1 GET /preflighted" }));
    EXPECT(!is_rooted(controller->token()));
}

// A request torn down before its response arrives leaves main fetch nothing to hand over.
TEST_CASE(a_fetch_stopped_or_aborted_before_its_response_ends_without_a_handover)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto& heap = harness.realm->heap();
    auto record = make_ref_counted<AlgorithmCalls>();
    auto stopped_request = harness.create_request_for("http://127.0.0.1:8000/stopped"sv);
    auto stopped = GC::make_root(harness.fetch(stopped_request, recording_responses(heap, record)));
    (void)request_server.wait_for_request("GET"sv);
    auto aborted_request = harness.create_request_for("http://127.0.0.1:8000/aborted"sv);
    auto aborted = GC::make_root(harness.fetch(aborted_request, recording_responses(heap, record)));
    (void)request_server.wait_for_request("GET"sv);

    EXPECT(is_rooted(stopped->token()));
    EXPECT(is_rooted(aborted->token()));
    harness.with_execution_context([&] {
        stopped->stop_fetch();
        aborted->abort(*harness.realm, {});
    });
    harness.run_until_step(commands, "end of fetch 2"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "start fetch 2", "stop 1", "abort 2", "end of fetch 1", "end of fetch 2" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /stopped", "start 1 GET /aborted", "stop 0", "stop 1" }));
    EXPECT(record->responses.is_empty());
    EXPECT(!is_rooted(stopped->token()));
    EXPECT(!is_rooted(aborted->token()));
}

namespace {

// A fetch that waits for a preload, which a test names once it has started, and each time the preload resolved it.
struct WaitingFetch : RefCounted<WaitingFetch> {
    GC_HOLDS_NO_CELL_EDGES;

    Optional<Web::Fetch::Infrastructure::FetchControllerToken> token;
    Vector<ByteString> resolutions;
};

}

// A preload of the URL, as HTMLLinkElement::preload makes one: once its body has been read, its entry keeps the
// response, or gives it to the fetch waiting on the entry. Stopping it cancels its entry.
static NEVER_INLINE Web::Fetch::Infrastructure::FetchControllerToken start_preload(FetchHarness const& harness, StringView url, NonnullRefPtr<WaitingFetch> const& waiting_fetch)
{
    auto entry = Web::HTML::PreloadEntry::create();
    auto request = harness.create_request_for(url);
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response_consume_body = GC::create_function(harness.realm->heap(), GC::captures(entry, waiting_fetch), [](auto& entry, auto& waiting_fetch, GC::Ref<ResponseView> response_view, FetchAlgorithms::BodyBytes body_bytes) {
        auto response = response_view->record()->clone();
        if (auto* byte_sequence = body_bytes.get_pointer<Core::ImmutableBytes>())
            response->unsafe_response()->set_body(Web::Fetch::Infrastructure::ResponseBody::from_bytes(*byte_sequence));
        else
            response = Response::network_error("Expected preload response to contain a body"_string);

        if (!entry->on_response_available) {
            if (!entry->cancelled)
                entry->response = response;
            return;
        }
        auto on_response_available = entry->on_response_available;
        entry->on_response_available = nullptr;
        on_response_available->function()(response);
        auto waiting_fetch_is_rooted = waiting_fetch->token.has_value() && is_rooted(*waiting_fetch->token);
        waiting_fetch->resolutions.append(ByteString::formatted("resolved by the preload's response, rooted: {}", waiting_fetch_is_rooted));
    });
    auto controller = harness.fetch(request, move(input));
    entry->controller = controller;
    controller->set_stop_steps(GC::create_function(harness.realm->heap(), GC::captures(entry, waiting_fetch), [](auto& entry, auto& waiting_fetch) {
        auto has_a_waiting_fetch = entry->on_response_available && !entry->response;
        entry->cancel();
        if (has_a_waiting_fetch)
            waiting_fetch->resolutions.append("resolved by a cancelled preload");
    }));
    harness.document->map_of_preloaded_resources().set(Web::HTML::create_a_preload_key(*request), entry);
    return controller->token();
}

static void run_until_handed_over(FetchHarness const& harness, Web::Fetch::Infrastructure::FetchControllerToken token)
{
    auto controller = find_controller(token);
    VERIFY(controller);
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(GC::Ref { *controller }), [](auto& controller) { return was_handed_over(*controller); }));
}

// The waiting fetch is the test's second. Its main fetch waits for the preload once the engine has started it.
static void run_until_waiting_for_preload(FetchHarness const& harness, NonnullRefPtr<ReachedSteps> const& commands)
{
    harness.run_until_step(commands, "start fetch 2"sv);
}

TEST_CASE(a_fetch_waiting_for_a_preload_is_resumed_by_its_engine)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto waiting_fetch = make_ref_counted<WaitingFetch>();
    (void)start_preload(harness, "http://127.0.0.1:8000/preloaded"sv, waiting_fetch);
    auto preload_id = request_server.wait_for_request("GET"sv);

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preloaded"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    waiting_fetch->token = controller->token();
    run_until_waiting_for_preload(harness, commands);
    EXPECT(is_rooted(controller->token()));

    // The preload's response reaches the engine, which resumes main fetch with it.
    request_server.send_headers(preload_id, 200);
    request_server.send_finished(preload_id);
    harness.run_until_responses(record, 1);
    EXPECT_EQ(waiting_fetch->resolutions, (Vector<ByteString> { "resolved by the preload's response, rooted: true" }));
    EXPECT(!record->responses[0]->is_network_error());
    harness.run_until_step(commands, "end of fetch 2"sv);
    EXPECT(commands->steps.contains_slow("preload response resolved 2"sv));
    EXPECT(!is_rooted(controller->token()));
}

namespace {

// Gives each fetch it starts a preloaded response candidate that is already a response.
class PreloadedResponseInjector final : public Web::Fetch::Engine::EngineCommandSink {
public:
    PreloadedResponseInjector(NonnullRefPtr<ReachedSteps> misbehaviors, NonnullRefPtr<Response> preloaded_response)
        : m_engine(Web::HTML::surrounding_agent().network_activity_observers, make<MisbehaviorRecordingAgent>(move(misbehaviors)))
        , m_preloaded_response(move(preloaded_response))
    {
    }

    virtual void attach_to_executor(Web::Fetch::Engine::FetchExecutor& executor) override { m_engine.attach_to_executor(executor); }

    virtual void handle_command(Web::Fetch::Engine::EngineCommand command) override
    {
        if (auto* start = command.get_pointer<Web::Fetch::Engine::Command::StartFetch>())
            start->input.main_fetch.fetch_params->set_preloaded_response_candidate(m_preloaded_response);
        m_engine.handle_command(move(command));
    }

private:
    Web::Fetch::Fetching::FetchEngine m_engine;
    NonnullRefPtr<Response> m_preloaded_response;
};

}

// Starts a fetch whose preloaded response has the body, and returns the misbehavior the engine saw.
static NonnullRefPtr<ReachedSteps> fetch_with_a_preloaded_response(FetchHarness const& harness, Web::Fetch::Infrastructure::ResponseBody body, NonnullRefPtr<AlgorithmCalls> const& record)
{
    auto misbehaviors = make_ref_counted<ReachedSteps>();
    auto preloaded_response = Response::create();
    preloaded_response->set_body(move(body));
    harness.run_fetches_on(make<PreloadedResponseInjector>(misbehaviors, preloaded_response));
    (void)harness.fetch(harness.create_request(), recording_responses(harness.realm->heap(), record));
    harness.settle();
    return misbehaviors;
}

TEST_CASE(a_fetch_started_with_a_preloaded_response_whose_body_is_not_all_its_bytes_is_misbehavior)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto record = make_ref_counted<AlgorithmCalls>();

    auto without_bytes = fetch_with_a_preloaded_response(harness, Web::Fetch::Infrastructure::ResponseBody { Core::ImmutableBytes {}, 0 }, record);
    auto shorter = fetch_with_a_preloaded_response(harness, Web::Fetch::Infrastructure::ResponseBody { MUST(Core::ImmutableBytes::copy("preloaded"sv.bytes())), 20 }, record);

    Vector<ByteString> expected { "The agent gave main fetch a preloaded response whose body is not all its bytes" };
    EXPECT_EQ(without_bytes->steps, expected);
    EXPECT_EQ(shorter->steps, expected);
    EXPECT(record->responses.is_empty());
}

TEST_CASE(a_fetch_started_with_a_preloaded_response_with_an_empty_body_hands_it_over)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto record = make_ref_counted<AlgorithmCalls>();

    auto misbehaviors = fetch_with_a_preloaded_response(harness, Web::Fetch::Infrastructure::ResponseBody::from_bytes(MUST(Core::ImmutableBytes::copy({}))), record);

    EXPECT(misbehaviors->steps.is_empty());
    EXPECT_EQ(record->responses.size(), 1u);
    EXPECT(!record->responses[0]->is_network_error());
}

TEST_CASE(a_fetch_aborted_while_it_waits_for_a_preload_ends_there)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto waiting_fetch = make_ref_counted<WaitingFetch>();
    (void)start_preload(harness, "http://127.0.0.1:8000/preloaded"sv, waiting_fetch);
    auto preload_id = request_server.wait_for_request("GET"sv);

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preloaded"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    waiting_fetch->token = controller->token();
    run_until_waiting_for_preload(harness, commands);
    harness.with_execution_context([&] { controller->abort(*harness.realm, {}); });
    harness.run_until_step(commands, "end of fetch 2"sv);
    EXPECT(!is_rooted(controller->token()));

    // The preload completes after the fetch has ended, and gives it nothing.
    request_server.send_headers(preload_id, 200);
    request_server.send_finished(preload_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(waiting_fetch->resolutions, (Vector<ByteString> { "resolved by the preload's response, rooted: false" }));
    EXPECT(record->responses.is_empty());
}

struct HeldWaitingFetch {
    GC::Root<FetchController> controller;
    Web::Fetch::Infrastructure::FetchControllerToken token;
};

// Starts a fetch of the preload's URL, lets its main fetch wait for the preload, then stops both fetches as a document's
// abort stops them. It runs away from the test's own stack frame, so a conservative scan of it finds no controller.
static NEVER_INLINE HeldWaitingFetch wait_for_a_preload_then_stop_both(FetchHarness const& harness, NonnullRefPtr<ReachedSteps> const& commands, StringView url, Web::Fetch::Infrastructure::FetchControllerToken preload_token, NonnullRefPtr<AlgorithmCalls> const& record)
{
    auto request = harness.create_request_for(url);
    auto controller = harness.fetch(request, recording_responses(harness.realm->heap(), record));
    run_until_waiting_for_preload(harness, commands);
    harness.with_execution_context([&] {
        find_controller(preload_token)->stop_fetch();
        controller->stop_fetch();
    });
    return { GC::make_root(controller), controller->token() };
}

// Both fetches are stopped as a document's abort stops them. The preload's stop cancels its entry, and the waiting
// fetch's own stop ends it before it fetches on its own.
static void stop_a_fetch_waiting_for_a_preload_and_the_preload(bool preload_body_in_flight)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();

    auto waiting_fetch = make_ref_counted<WaitingFetch>();
    auto preload_token = start_preload(harness, "http://127.0.0.1:8000/preloaded"sv, waiting_fetch);
    auto preload_id = request_server.wait_for_request("GET"sv);
    if (preload_body_in_flight) {
        request_server.send_headers(preload_id, 200);
        run_until_handed_over(harness, preload_token);
    }

    // The test holds the waiting fetch, as its fetch group holds a keepalive fetch.
    auto record = make_ref_counted<AlgorithmCalls>();
    auto waiting = wait_for_a_preload_then_stop_both(harness, commands, "http://127.0.0.1:8000/preloaded"sv, preload_token, record);

    // The waiting fetch ends at its own stop, without a request or a handover.
    harness.run_until_step(commands, "end of fetch 2"sv);
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "start fetch 2", "stop 1", "preload response resolved 2", "stop 2", "end of fetch 1", "end of fetch 2" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /preloaded", "stop 0" }));
    EXPECT_EQ(waiting_fetch->resolutions, (Vector<ByteString> { "resolved by a cancelled preload" }));
    EXPECT(record->responses.is_empty());
    EXPECT(!is_rooted(waiting.token));

    // With nothing else holding the fetch, its engine record leaves the engine's table.
    waiting.controller = {};
    GC::scrub_stack_for_testing();
    harness.run_tasks_until(GC::create_function(heap, GC::captures(harness.realm, harness.records_gone), [](auto& realm, auto& records_gone) {
        realm->heap().collect_garbage();
        return records_gone->steps.contains_slow("2"sv);
    }));
    EXPECT(harness.records_gone->steps.contains_slow("2"sv));
}

TEST_CASE(a_fetch_waiting_for_a_preload_stopped_before_its_response_ends_when_it_is_stopped)
{
    stop_a_fetch_waiting_for_a_preload_and_the_preload(false);
}

TEST_CASE(a_fetch_waiting_for_a_preload_stopped_with_its_body_in_flight_ends_when_it_is_stopped)
{
    stop_a_fetch_waiting_for_a_preload_and_the_preload(true);
}

// A fetch that took the preload's entry, but whose main fetch's earlier steps set a network error, never waits.
TEST_CASE(a_fetch_whose_main_fetch_never_waits_for_its_preload_ignores_the_preload_s_cancellation)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto waiting_fetch = make_ref_counted<WaitingFetch>();
    auto preload_token = start_preload(harness, "http://127.0.0.1:8000/preloaded"sv, waiting_fetch);
    (void)request_server.wait_for_request("GET"sv);

    // The fetch's start and the preload's cancellation reach the engine before main fetch hands over its network error.
    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preloaded"sv);
    request->set_local_urls_only(true);
    harness.with_execution_context([&] {
        auto controller = harness.fetch(request, recording_responses(harness.realm->heap(), record));
        waiting_fetch->token = controller->token();
        find_controller(preload_token)->stop_fetch();
    });

    harness.run_until_responses(record, 1);
    EXPECT(record->responses[0]->is_network_error());
    harness.run_until_step(commands, "end of fetch 2"sv);
    harness.settle();
    EXPECT_EQ(waiting_fetch->resolutions, (Vector<ByteString> { "resolved by a cancelled preload" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /preloaded", "stop 0" }));
}

static void stop_the_preload_a_fetch_waits_for(bool preload_body_in_flight)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto waiting_fetch = make_ref_counted<WaitingFetch>();
    auto preload_token = start_preload(harness, "http://127.0.0.1:8000/preloaded"sv, waiting_fetch);
    auto preload_id = request_server.wait_for_request("GET"sv);
    if (preload_body_in_flight) {
        request_server.send_headers(preload_id, 200);
        run_until_handed_over(harness, preload_token);
    }

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preloaded"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    waiting_fetch->token = controller->token();
    run_until_waiting_for_preload(harness, commands);

    // The preload's stop cancels its entry, and the waiting fetch fetches on its own, and reports its own timing.
    harness.with_execution_context([&] { find_controller(preload_token)->stop_fetch(); });
    EXPECT(!controller->preload_reports_timing());
    auto own_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(own_id, 200);
    request_server.send_finished(own_id);
    harness.run_until_responses(record, 1);
    EXPECT_EQ(waiting_fetch->resolutions, (Vector<ByteString> { "resolved by a cancelled preload" }));
    EXPECT(!record->responses[0]->is_network_error());
    harness.run_until_step(commands, "end of fetch 2"sv);
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_fetch_waiting_for_a_preload_stopped_before_its_response_fetches_on_its_own)
{
    stop_the_preload_a_fetch_waits_for(false);
}

TEST_CASE(a_fetch_waiting_for_a_preload_stopped_with_its_body_in_flight_fetches_on_its_own)
{
    stop_the_preload_a_fetch_waits_for(true);
}

namespace {

// The interceptions the engine offered, in the order it offered them.
struct OfferedInterceptions : RefCounted<OfferedInterceptions> {
    Vector<Web::Fetch::Engine::ContinuationId> ids;
};

// Records the interception each network phase event offers, and ignores every other event.
class InterceptionRecordingEventSink final : public Web::Fetch::Engine::EngineEventSink {
public:
    explicit InterceptionRecordingEventSink(NonnullRefPtr<OfferedInterceptions> offered)
        : m_offered(move(offered))
    {
    }

    virtual void handle_event(Web::Fetch::Engine::EngineEvent event) override
    {
        using Web::Fetch::Engine::Event::FetchNetworkActivity;
        auto const* activity = event.get_pointer<FetchNetworkActivity>();
        if (!activity)
            return;
        activity->activity.visit(
            [&](OneOf<FetchNetworkActivity::BeforeRequestSent, FetchNetworkActivity::ResponseStarted, FetchNetworkActivity::AuthRequired> auto const& phase) {
                if (phase.interception.has_value())
                    m_offered->ids.append(*phase.interception);
            },
            [](auto const&) { });
    }

private:
    NonnullRefPtr<OfferedInterceptions> m_offered;
};

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

// Has the WebDriver BiDi session block each request before it is sent, as an intercept for that phase does.
static void block_each_request_before_it_is_sent()
{
    auto session_state = Web::WebDriver::BiDi::session_state();
    session_state.enabled_event_contexts.set("network.beforeRequestSent"_string, {});
    session_state.network_intercepts.append({ .id = "intercept"_string, .url_patterns = {}, .phases = { "beforeRequestSent"_string }, .browsing_contexts = {} });
    Web::WebDriver::BiDi::set_session_state(move(session_state));
}

// Records the id of each request the session blocks.
static WebDriverBiDiSession::OnEvent recording_blocked_requests(NonnullRefPtr<ReportedNetworkEvents> const& events)
{
    return [events](Web::HTML::Navigable const&, String const&, JsonValue params) {
        if (params.as_object().get_bool("isBlocked"sv).value_or(false))
            events->request_ids.append(*params.as_object().get_object("request"sv)->get_string("request"sv));
    };
}

static void run_until_blocked(FetchHarness const& harness, NonnullRefPtr<ReportedNetworkEvents> const& blocked)
{
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(blocked), [](auto& blocked) { return !blocked->request_ids.is_empty(); }));
    VERIFY(blocked->request_ids.size() == 1);
}

TEST_CASE(a_fetch_whose_request_webdriver_bidi_blocks_ends_once_a_command_resumes_it)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked) };
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/blocked"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    run_until_blocked(harness, blocked);
    harness.settle();

    // The blocked fetch has started no request, and is rooted while it waits.
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1" }));
    EXPECT(request_server.messages().is_empty());
    EXPECT(is_rooted(controller->token()));

    JsonObject params;
    params.set("request"sv, blocked->request_ids[0]);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params).is_error());
    });
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200);
    harness.run_until_responses(record, 1);
    request_server.send_finished(request_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "resolve network interception 1", "end of body of 1", "end of fetch 1" }));
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_fetch_whose_request_webdriver_bidi_blocks_ends_once_aborted)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked) };
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/blocked"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    run_until_blocked(harness, blocked);
    EXPECT(is_rooted(controller->token()));

    // The fetch hands its network error over without sending its request.
    harness.with_execution_context([&] { controller->abort(*harness.realm, {}); });
    harness.run_until_responses(record, 1);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT(record->responses[0]->is_aborted_network_error());
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1" }));
    EXPECT(request_server.messages().is_empty());
    EXPECT(!is_rooted(controller->token()));
}

TEST_CASE(a_webdriver_bidi_command_cannot_give_a_blocked_request_a_forbidden_method)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked) };
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/forbidden-method"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    run_until_blocked(harness, blocked);

    JsonObject params;
    params.set("request"sv, blocked->request_ids[0]);
    params.set("method"sv, "CONNECT"sv);
    harness.with_execution_context([&] {
        auto result = Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params);
        VERIFY(result.is_error());
        EXPECT_EQ(result.error().error, "invalid argument"sv);
    });

    // The request stays blocked, unsent, until the fetch is aborted.
    harness.with_execution_context([&] { controller->abort(*harness.realm, {}); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1" }));
    EXPECT(request_server.messages().is_empty());
}

TEST_CASE(a_webdriver_bidi_command_cannot_give_a_blocked_request_a_url_that_is_not_http)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked) };
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/not-http-url"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    run_until_blocked(harness, blocked);

    JsonObject params;
    params.set("request"sv, blocked->request_ids[0]);
    params.set("url"sv, "file:///"sv);
    harness.with_execution_context([&] {
        auto result = Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params);
        VERIFY(result.is_error());
        EXPECT_EQ(result.error().error, "invalid argument"sv);
    });

    // The request stays blocked, unsent, until the fetch is aborted.
    harness.with_execution_context([&] { controller->abort(*harness.realm, {}); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1" }));
    EXPECT(request_server.messages().is_empty());
}

TEST_CASE(the_end_of_a_webdriver_bidi_session_settles_a_blocked_request_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    Optional<WebDriverBiDiSession> session;
    session.emplace(Vector<String> { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked));
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/blocked-until-the-session-ends"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    run_until_blocked(harness, blocked);

    // The session's end continues the request, which is then sent.
    session.clear();
    (void)request_server.wait_for_request("GET"sv);
    JsonObject params;
    params.set("request"sv, blocked->request_ids[0]);
    harness.with_execution_context([&] {
        auto result = Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params);
        EXPECT(result.is_error());
    });

    // A stop after the request was continued settles nothing more.
    harness.with_execution_context([&] { controller->stop_fetch(); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "resolve network interception 1", "stop 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /blocked-until-the-session-ends", "stop 0" }));
}

// The network interceptions that WebDriver BiDi's network events name, as blocking their request or as ended.
struct NamedInterceptions : RefCounted<NamedInterceptions> {
    GC_HOLDS_NO_CELL_EDGES;

    Vector<u64> blocking;
    Vector<u64> ended;
    Vector<String> blocked_request_ids;
};

static WebDriverBiDiSession::OnEvent recording_named_interceptions(NonnullRefPtr<NamedInterceptions> const& interceptions)
{
    return [interceptions](Web::HTML::Navigable const&, String const&, JsonValue params) {
        auto const& object = params.as_object();
        auto interception = object.get_u64(Web::WebDriver::Network::network_interception_parameter);
        if (object.get_bool("isBlocked"sv).value_or(false)) {
            interceptions->blocking.append(interception.value());
            interceptions->blocked_request_ids.append(*object.get_object("request"sv)->get_string("request"sv));
        } else if (interception.has_value()) {
            interceptions->ended.append(*interception);
        }
    };
}

TEST_CASE(a_stop_of_one_fetch_of_a_request_leaves_another_of_its_fetches_blocked)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();

    auto interceptions = make_ref_counted<NamedInterceptions>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string, "network.fetchError"_string }, recording_named_interceptions(interceptions) };
    block_each_request_before_it_is_sent();

    auto request = harness.create_request_for("http://127.0.0.1:8000/fetched-twice-at-once"sv);
    auto blocked_record = make_ref_counted<AlgorithmCalls>();
    auto blocked_controller = GC::make_root(harness.fetch(request, recording_responses(heap, blocked_record)));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(interceptions), [](auto& interceptions) { return !interceptions->blocking.is_empty(); }));
    VERIFY(interceptions->blocking.size() == 1);

    // The request's second fetch shares its WebDriver BiDi id, so it is not blocked again, and is sent.
    auto stopped_record = make_ref_counted<AlgorithmCalls>();
    auto stopped_controller = GC::make_root(harness.fetch(request, recording_responses(heap, stopped_record)));
    (void)request_server.wait_for_request("GET"sv);
    harness.with_execution_context([&] { stopped_controller->stop_fetch(); });
    harness.run_until_step(commands, "end of fetch 2"sv);
    harness.settle();

    // The second fetch's error ends no interception, so the browser keeps routing the first fetch's block.
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "start fetch 2", "resolve network interception 2", "stop 2", "end of fetch 2" }));
    EXPECT(interceptions->ended.is_empty());
    EXPECT(blocked_record->responses.is_empty());

    JsonObject params;
    params.set("request"sv, interceptions->blocked_request_ids[0]);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params).is_error());
    });
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200);
    request_server.send_finished(request_id);
    harness.run_until_responses(blocked_record, 1);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
}

TEST_CASE(a_stop_of_a_blocked_fetch_ends_its_interception_after_another_fetch_of_its_request_reported_an_error)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();

    auto interceptions = make_ref_counted<NamedInterceptions>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string, "network.fetchError"_string }, recording_named_interceptions(interceptions) };
    block_each_request_before_it_is_sent();

    auto request = harness.create_request_for("http://127.0.0.1:8000/fetched-twice-and-stopped"sv);
    auto blocked_record = make_ref_counted<AlgorithmCalls>();
    auto blocked_controller = GC::make_root(harness.fetch(request, recording_responses(heap, blocked_record)));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(interceptions), [](auto& interceptions) { return !interceptions->blocking.is_empty(); }));
    VERIFY(interceptions->blocking.size() == 1);

    // The request's second fetch reports the request's error first.
    auto stopped_record = make_ref_counted<AlgorithmCalls>();
    auto stopped_controller = GC::make_root(harness.fetch(request, recording_responses(heap, stopped_record)));
    (void)request_server.wait_for_request("GET"sv);
    harness.with_execution_context([&] { stopped_controller->stop_fetch(); });
    harness.run_until_step(commands, "end of fetch 2"sv);
    EXPECT(interceptions->ended.is_empty());

    // The stop of the fetch that holds the block still ends its interception, so the browser stops routing it.
    harness.with_execution_context([&] { blocked_controller->stop_fetch(); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(interceptions->ended, interceptions->blocking);
    EXPECT(blocked_record->responses.is_empty());
}

TEST_CASE(a_fetch_of_a_request_that_another_fetch_holds_blocked_goes_on_at_a_later_phase)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string, "network.responseStarted"_string }, recording_blocked_requests(blocked) };
    auto session_state = Web::WebDriver::BiDi::session_state();
    session_state.enabled_event_contexts.set("network.beforeRequestSent"_string, {});
    session_state.enabled_event_contexts.set("network.responseStarted"_string, {});
    session_state.network_intercepts.append({ .id = "intercept"_string, .url_patterns = {}, .phases = { "beforeRequestSent"_string, "responseStarted"_string }, .browsing_contexts = {} });
    Web::WebDriver::BiDi::set_session_state(move(session_state));

    auto request = harness.create_request_for("http://127.0.0.1:8000/fetched-twice-at-two-phases"sv);
    auto blocked_record = make_ref_counted<AlgorithmCalls>();
    auto blocked_controller = GC::make_root(harness.fetch(request, recording_responses(heap, blocked_record)));
    run_until_blocked(harness, blocked);
    auto blocked_request_id = blocked->request_ids[0];

    // The second fetch reaches a phase the session intercepts while the first holds the request blocked.
    auto other_record = make_ref_counted<AlgorithmCalls>();
    auto other_controller = GC::make_root(harness.fetch(request, recording_responses(heap, other_record)));
    auto other_request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(other_request_id, 200);
    request_server.send_finished(other_request_id);
    harness.run_until_responses(other_record, 1);
    harness.run_until_step(commands, "end of fetch 2"sv);

    // The first fetch is still blocked, and goes on once it is continued.
    JsonObject params;
    params.set("request"sv, blocked_request_id);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params).is_error());
    });
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200);
    request_server.send_finished(request_id);
    harness.run_until_responses(blocked_record, 1);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
}

TEST_CASE(a_webdriver_bidi_answer_with_credentials_also_overrides_the_response)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto started = harness.record_started_fetches();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.authRequired"_string }, recording_blocked_requests(blocked) };
    auto session_state = Web::WebDriver::BiDi::session_state();
    session_state.enabled_event_contexts.set("network.authRequired"_string, {});
    session_state.network_intercepts.append({ .id = "intercept"_string, .url_patterns = {}, .phases = { "authRequired"_string }, .browsing_contexts = {} });
    Web::WebDriver::BiDi::set_session_state(move(session_state));

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/asks-for-credentials"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);
    VERIFY(started->fetch_params.size() == 1);

    // The fetch's request reaches the phase with a response that asks for credentials.
    auto response = Response::create();
    response->set_status(401);
    response->header_list()->append({ "WWW-Authenticate"sv, "Basic realm=\"test\""sv });
    auto continued = make_ref_counted<HandedOverResponses>();
    VERIFY(Web::Fetch::Fetching::can_wait_at_network_phase(started->fetch_params[0], Web::Fetch::Engine::NetworkPhase::AuthRequired));
    Web::Fetch::Fetching::wait_at_network_phase(started->fetch_params[0], Web::Fetch::Engine::NetworkPhase::AuthRequired, response, nullptr, [continued](auto const&, RefPtr<Response> continued_response) {
        continued->responses.append(continued_response.release_nonnull());
    });
    run_until_blocked(harness, blocked);

    // One command gives the credentials and replaces the response's headers.
    auto params = MUST(JsonValue::from_string(R"({"credentials":{"type":"password","username":"user","password":"pass"},"headers":[{"name":"X-Overridden","value":{"type":"string","value":"1"}}]})"sv)).as_object();
    params.set("request"sv, blocked->request_ids[0]);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueResponse"sv, params).is_error());
    });
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(continued), [](auto& continued) { return !continued->responses.is_empty(); }));
    VERIFY(continued->responses.size() == 1);

    EXPECT_EQ(continued->responses[0]->header_list()->get("X-Overridden"sv).value_or({}), "1"sv);
    EXPECT_EQ(request->header_list()->get("Authorization"sv).value_or({}), "Basic dXNlcjpwYXNz"sv);

    request_server.send_headers(request_id, 200);
    request_server.send_finished(request_id);
    harness.run_until_responses(record, 1);
}

TEST_CASE(a_webdriver_bidi_answer_with_credentials_fetches_the_request_again_with_them)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.authRequired"_string }, recording_blocked_requests(blocked) };
    auto session_state = Web::WebDriver::BiDi::session_state();
    session_state.enabled_event_contexts.set("network.authRequired"_string, {});
    session_state.network_intercepts.append({ .id = "intercept"_string, .url_patterns = {}, .phases = { "authRequired"_string }, .browsing_contexts = {} });
    Web::WebDriver::BiDi::set_session_state(move(session_state));

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/needs-credentials"sv);
    request->set_credentials_mode(Request::CredentialsMode::Include);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto first_request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(first_request_id, 401, { { "WWW-Authenticate"sv, "Basic realm=\"test\""sv } });
    run_until_blocked(harness, blocked);

    auto params = MUST(JsonValue::from_string(R"({"credentials":{"type":"password","username":"user","password":"pass"},"headers":[{"name":"X-Overridden","value":{"type":"string","value":"1"}}]})"sv)).as_object();
    params.set("request"sv, blocked->request_ids[0]);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueResponse"sv, params).is_error());
    });

    // The request goes again with the credentials, and the fetch's response answers that, so the headers that replaced
    // those of the 401 go with the 401.
    auto second_request_id = request_server.wait_for_request("GET"sv);
    EXPECT_EQ(request_server.request_header(second_request_id, "Authorization"sv).value_or({}), "Basic dXNlcjpwYXNz"sv);
    request_server.send_headers(second_request_id, 200);
    request_server.send_finished(second_request_id);
    harness.run_until_responses(record, 1);
    auto response = record->responses[0]->record()->unsafe_response();
    EXPECT_EQ(response->status(), 200);
    EXPECT(!response->header_list()->contains("X-Overridden"sv));
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
}

TEST_CASE(a_header_webdriver_bidi_changes_before_a_request_is_sent_selects_the_stored_response)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();
    auto memory_cache_was_enabled = Web::Fetch::Fetching::http_memory_cache_enabled();
    Web::Fetch::Fetching::set_http_memory_cache_enabled(true);
    ScopeGuard restore_memory_cache = [&] { Web::Fetch::Fetching::set_http_memory_cache_enabled(memory_cache_was_enabled); };

    // The harness's document has an origin of the network, so its requests have an HTTP cache.
    auto origin = URL::Parser::basic_parse("http://127.0.0.1:8000/"sv)->origin();
    harness.document->set_origin(origin);
    harness.document->relevant_settings_object().top_level_origin = origin;

    auto url = "http://127.0.0.1:8000/varies-by-language"sv;
    auto fetch_in = [&](StringView language, NonnullRefPtr<AlgorithmCalls> const& record) {
        auto request = harness.create_request_for(url);
        request->header_list()->set({ "Accept-Language"sv, language });
        (void)harness.fetch(request, recording_responses(heap, record));
        return request;
    };

    // The cache holds a fresh response for each of two languages.
    for (auto language : { "en"sv, "fr"sv }) {
        auto record = make_ref_counted<AlgorithmCalls>();
        auto request = fetch_in(language, record);
        auto request_id = request_server.wait_for_request("GET"sv);
        request_server.send_headers(request_id, 200, { { "Cache-Control"sv, "max-age=3600"sv }, { "Vary"sv, "Accept-Language"sv }, { "Content-Language"sv, language } });
        request_server.send_finished(request_id);
        harness.run_tasks_until(GC::create_function(heap, GC::captures(request), [](auto& request) { return request->done(); }));
    }

    auto blocked = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked) };
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    (void)fetch_in("en"sv, record);
    run_until_blocked(harness, blocked);

    // The command changes the request's language before the cache is consulted.
    auto params = MUST(JsonValue::from_string(R"({"headers":[{"name":"Accept-Language","value":{"type":"string","value":"fr"}}]})"sv)).as_object();
    params.set("request"sv, blocked->request_ids[0]);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params).is_error());
    });
    harness.run_until_responses(record, 1);
    harness.settle();

    auto response = record->responses[0]->record()->unsafe_response();
    EXPECT(response->cache_state() == Response::CacheState::Local);
    EXPECT_EQ(response->header_list()->get("Content-Language"sv).value_or({}), "fr"sv);
    size_t started_requests = 0;
    for (auto const& message : request_server.messages()) {
        if (message.starts_with("start "sv))
            ++started_requests;
    }
    EXPECT_EQ(started_requests, 2u);
}

TEST_CASE(a_blocked_request_aborted_after_it_is_continued_looks_up_no_stored_response)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();
    auto memory_cache_was_enabled = Web::Fetch::Fetching::http_memory_cache_enabled();
    Web::Fetch::Fetching::set_http_memory_cache_enabled(true);
    ScopeGuard restore_memory_cache = [&] { Web::Fetch::Fetching::set_http_memory_cache_enabled(memory_cache_was_enabled); };

    // The harness's document has an origin of the network, so its requests have an HTTP cache.
    auto origin = URL::Parser::basic_parse("http://127.0.0.1:8000/"sv)->origin();
    harness.document->set_origin(origin);
    harness.document->relevant_settings_object().top_level_origin = origin;

    // The cache holds a response that is stale at once. A lookup drops it, unless the request takes a stale response.
    auto url = "http://127.0.0.1:8000/stale-at-once"sv;
    {
        auto record = make_ref_counted<AlgorithmCalls>();
        auto request = harness.create_request_for(url);
        (void)harness.fetch(request, recording_responses(heap, record));
        auto request_id = request_server.wait_for_request("GET"sv);
        request_server.send_headers(request_id, 200, { { "Cache-Control"sv, "max-age=0"sv } });
        request_server.send_finished(request_id);
        harness.run_tasks_until(GC::create_function(heap, GC::captures(request), [](auto& request) { return request->done(); }));
    }

    // The blocked request is continued, then aborted, before its fetch goes on.
    {
        auto blocked = make_ref_counted<ReportedNetworkEvents>();
        WebDriverBiDiSession session { { "network.beforeRequestSent"_string }, recording_blocked_requests(blocked) };
        block_each_request_before_it_is_sent();

        auto record = make_ref_counted<AlgorithmCalls>();
        auto controller = GC::make_root(harness.fetch(harness.create_request_for(url), recording_responses(heap, record)));
        run_until_blocked(harness, blocked);

        JsonObject params;
        params.set("request"sv, blocked->request_ids[0]);
        harness.with_execution_context([&] {
            EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params).is_error());
            controller->abort(*harness.realm, {});
        });
        harness.run_until_responses(record, 1);
        VERIFY(record->responses.size() == 1);
        EXPECT(record->responses[0]->is_aborted_network_error());
    }

    // The stored response is still there, so a request that takes a stale response gets it without the network.
    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for(url);
    request->set_cache_mode(HTTP::CacheMode::ForceCache);
    (void)harness.fetch(request, recording_responses(heap, record));
    harness.run_until_responses(record, 1);
    VERIFY(record->responses.size() == 1);
    EXPECT(record->responses[0]->record()->unsafe_response()->cache_state() == Response::CacheState::Local);
    size_t started_requests = 0;
    for (auto const& message : request_server.messages()) {
        if (message.starts_with("start "sv))
            ++started_requests;
    }
    EXPECT_EQ(started_requests, 1u);
}

TEST_CASE(only_the_event_of_the_phase_a_late_session_blocks_at_reports_the_block)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/blocked-at-response-started"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);

    // A session that starts while the request is in flight intercepts its response only.
    auto events = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session {
        { "network.beforeRequestSent"_string, "network.responseStarted"_string },
        [events](Web::HTML::Navigable const&, String const& name, JsonValue params) {
            auto const& object = params.as_object();
            auto is_blocked = object.get_bool("isBlocked"sv).value();
            events->names.append(MUST(String::formatted("{} {}", name, is_blocked ? "blocked"sv : "not blocked"sv)));
            events->request_ids.append(*object.get_object("request"sv)->get_string("request"sv));
        },
    };
    auto session_state = Web::WebDriver::BiDi::session_state();
    session_state.enabled_event_contexts.set("network.responseStarted"_string, {});
    session_state.network_intercepts.append({ .id = "intercept"_string, .url_patterns = {}, .phases = { "responseStarted"_string }, .browsing_contexts = {} });
    Web::WebDriver::BiDi::set_session_state(move(session_state));

    request_server.send_headers(request_id, 200);
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(events), [](auto& events) { return events->names.size() == 2; }));
    EXPECT_EQ(events->names, (Vector<String> { "network.beforeRequestSent not blocked"_string, "network.responseStarted blocked"_string }));

    JsonObject params;
    params.set("request"sv, events->request_ids[1]);
    harness.with_execution_context([&] {
        EXPECT(!Web::WebDriver::BiDi::continue_network_request("network.continueResponse"sv, params).is_error());
    });
    harness.run_until_responses(record, 1);
    request_server.send_finished(request_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
}

TEST_CASE(a_cors_preflight_reports_its_network_events_under_the_request_id_of_its_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto events = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session {
        { "network.beforeRequestSent"_string, "network.responseCompleted"_string },
        [events](Web::HTML::Navigable const&, String const& name, JsonValue params) {
            auto const& request = *params.as_object().get_object("request"sv);
            events->names.append(MUST(String::formatted("{} {}", name, *request.get_string("method"sv))));
            events->request_ids.append(*request.get_string("request"sv));
        },
    };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preflighted-with-events"sv);
    request->set_mode(Request::Mode::CORS);
    request->set_use_cors_preflight(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto preflight_id = request_server.wait_for_request("OPTIONS"sv);
    request_server.send_headers(preflight_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_finished(request_id);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(request), [](auto& request) { return request->done(); }));

    EXPECT_EQ(events->names, (Vector<String> { "network.beforeRequestSent OPTIONS"_string, "network.responseCompleted OPTIONS"_string, "network.beforeRequestSent GET"_string, "network.responseCompleted GET"_string }));
    VERIFY(events->request_ids.size() == 4);
    EXPECT_EQ(events->request_ids[1], events->request_ids[0]);
    EXPECT_EQ(events->request_ids[3], events->request_ids[2]);
    EXPECT_EQ(events->request_ids[2], events->request_ids[0]);
}

TEST_CASE(a_stop_of_a_fetch_whose_cors_preflight_is_blocked_ends_the_preflight_s_interception)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();

    auto interceptions = make_ref_counted<NamedInterceptions>();
    WebDriverBiDiSession session { { "network.beforeRequestSent"_string, "network.fetchError"_string }, recording_named_interceptions(interceptions) };
    block_each_request_before_it_is_sent();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/preflight-blocked"sv);
    request->set_mode(Request::Mode::CORS);
    request->set_use_cors_preflight(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(heap, record)));
    harness.run_tasks_until(GC::create_function(heap, GC::captures(interceptions), [](auto& interceptions) { return !interceptions->blocking.is_empty(); }));
    VERIFY(interceptions->blocking.size() == 1);

    // The stop names the interception that blocks the preflight, so the browser stops routing the request to it.
    harness.with_execution_context([&] { controller->stop_fetch(); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();
    EXPECT_EQ(interceptions->ended, interceptions->blocking);

    // A late answer for the request finds no block, so no fetch gets it.
    JsonObject params;
    params.set("request"sv, interceptions->blocked_request_ids[0]);
    harness.with_execution_context([&] {
        EXPECT(Web::WebDriver::BiDi::continue_network_request("network.continueRequest"sv, params).is_error());
    });
    harness.settle();
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "stop 1", "end of fetch 1" }));
    EXPECT(request_server.messages().is_empty());
}

TEST_CASE(a_stop_of_a_fetch_whose_request_is_done_reports_nothing_to_webdriver_bidi)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request();
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(request), [](auto& request) { return request->done(); }));

    // A session that starts after the fetch hears nothing of the fetch's stop.
    auto events = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session {
        { "network.beforeRequestSent"_string, "network.fetchError"_string },
        [events](Web::HTML::Navigable const&, String const& name, JsonValue) { events->names.append(name); },
    };
    harness.with_execution_context([&] {
        controller->abort(*harness.realm, {});
        controller->stop_fetch();
    });
    harness.settle();
    EXPECT(events->names.is_empty());
}

TEST_CASE(a_network_interception_is_offered_and_answered_on_a_thread_that_forbids_heap_access)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto& registry = Web::HTML::surrounding_agent().fetch_controller_registry;
    auto controller = GC::make_root(FetchController::create(registry));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto token = controller->token();
    IGNORE_USE_IN_ESCAPING_LAMBDA auto weak_registry = registry.make_weak_ptr();
    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<ByteString> continued;

    auto thread = Threading::Thread::construct("FetchEngine"sv, [&]() -> intptr_t {
        GC::forbid_heap_access_on_this_thread();
        Web::Fetch::Engine::FetchEngineRecords records { make<Web::Fetch::Engine::FetchEngineRecords::Agent>() };
        auto observers = Web::Fetch::Engine::NetworkActivityObservers::create();
        observers->set_fetch_observation(true, Vector { Web::Fetch::Engine::NetworkPhase::BeforeRequestSent, Web::Fetch::Engine::NetworkPhase::ResponseStarted });
        auto offered = make_ref_counted<OfferedInterceptions>();
        auto record = records.start(Web::Fetch::Engine::FetchId { 1 }, make<InterceptionRecordingEventSink>(offered), observers).release_nonnull();

        auto request = Request::create();
        request->set_url(URL::Parser::basic_parse("https://example.com/"sv).release_value());
        auto fetch_params = FetchParams::create(token, weak_registry, Web::Fetch::Infrastructure::FetchControlBlock::create(), request, Web::Fetch::Infrastructure::FetchTimingInfo::create(), Web::current_loader_config());
        fetch_params->set_engine_record(record);

        VERIFY(Web::Fetch::Fetching::can_wait_at_network_phase(fetch_params, Web::Fetch::Engine::NetworkPhase::BeforeRequestSent));
        Web::Fetch::Fetching::wait_at_network_phase(fetch_params, Web::Fetch::Engine::NetworkPhase::BeforeRequestSent, nullptr, request, [&](auto const&, RefPtr<Response>) {
            continued.append(request->header_list()->get("X-Intercepted"sv).value_or({}));
        });
        VERIFY(offered->ids.size() == 1);
        Web::Fetch::Engine::ModifyRequest modified_request;
        modified_request.headers = Vector<HTTP::Header> { { "X-Intercepted"sv, "1"sv } };
        records.resolve_network_interception({ .fetch_id = Web::Fetch::Engine::FetchId { 1 }, .continuation_id = offered->ids[0], .decision = move(modified_request) });

        auto response = Response::create();
        response->set_status(200);
        VERIFY(Web::Fetch::Fetching::can_wait_at_network_phase(fetch_params, Web::Fetch::Engine::NetworkPhase::ResponseStarted));
        Web::Fetch::Fetching::wait_at_network_phase(fetch_params, Web::Fetch::Engine::NetworkPhase::ResponseStarted, response, nullptr, [&](auto const&, RefPtr<Response> continued_response) {
            continued.append(ByteString::number(continued_response->status()));
        });
        VERIFY(offered->ids.size() == 2);
        Web::Fetch::Engine::ModifyResponse modified_response;
        modified_response.status = 204;
        records.resolve_network_interception({ .fetch_id = Web::Fetch::Engine::FetchId { 1 }, .continuation_id = offered->ids[1], .decision = move(modified_response) });
        return 0;
    });
    thread->start();
    MUST(thread->join());

    EXPECT_EQ(continued, (Vector<ByteString> { "1", "204" }));
}

static JS::Value run_script(FetchHarness const& harness, StringView source)
{
    auto source_text = Utf16String::from_utf8_without_validation(source.bytes());
    auto script = JS::Script::parse(source_text.utf16_view(), *harness.realm);
    VERIFY(!script.is_error());
    auto result = harness.realm->vm().run(script.value());
    if (result.is_throw_completion()) {
        FAIL("the script threw");
        return JS::js_undefined();
    }
    return result.release_value();
}

// A sync XHR runs no task while it waits for its response, so the tasks of a fetch that ends meanwhile wait too.
TEST_CASE(a_fetch_that_ends_while_a_sync_xhr_waits_ends_once_and_runs_its_algorithms_afterwards)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto& heap = harness.realm->heap();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto input = recording_responses(heap, record);
    input.process_response_end_of_body = GC::create_function(heap, GC::captures(record), [](auto& record, GC::Ref<ResponseView>) {
        record->calls.append("process response end of body"sv);
    });
    auto request = harness.create_request_for("http://127.0.0.1:8000/ends-during-sync-xhr"sv);
    auto token = harness.fetch(request, move(input))->token();
    auto request_id = request_server.wait_for_request("GET"sv);

    // Once the XHR's request has started, the fetch's response arrives whole. Once the fetch has ended, the heap
    // collects its garbage and the XHR's response arrives. Ten seconds after the XHR starts, each arrives regardless.
    struct {
        bool answered_fetch { false };
        bool answered_xhr { false };
        bool fetch_ended { false };
        bool rooted_at_end { true };
        size_t algorithm_calls_at_end { 0 };
        size_t ticks { 0 };
    } seen;
    auto timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [&] {
        if (seen.answered_xhr)
            return;
        auto out_of_time = ++seen.ticks >= 10'000;
        if (!seen.answered_fetch) {
            if (!request_server.messages().contains_slow("start 1 GET /sync-xhr"sv) && !out_of_time)
                return;
            seen.answered_fetch = true;
            request_server.send_headers(request_id, 200);
            request_server.send_finished(request_id);
            return;
        }
        if (commands->steps.contains_slow("end of fetch 1"sv)) {
            seen.fetch_ended = true;
            seen.rooted_at_end = is_rooted(token);
            seen.algorithm_calls_at_end = record->responses.size() + record->calls.size();
            heap.collect_garbage_and_return_memory_to_system();
        } else if (!out_of_time) {
            return;
        }
        seen.answered_xhr = true;
        request_server.send_headers(1, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
        request_server.send_finished(1);
    });
    timer->start();
    auto status = run_script(harness, "const xhr = new XMLHttpRequest(); xhr.open('GET', 'http://127.0.0.1:8000/sync-xhr', false); xhr.send(); xhr.status;"sv);
    timer->stop();

    // The fetch ended inside the XHR's nested event loop, before any of its algorithms ran.
    EXPECT(status.is_number() && status.as_double() == 200);
    EXPECT(seen.fetch_ended);
    EXPECT(!seen.rooted_at_end);
    EXPECT_EQ(seen.algorithm_calls_at_end, 0u);

    // Its algorithms run once the event loop runs tasks again.
    harness.run_tasks_until(GC::create_function(heap, GC::captures(record), [](auto& record) { return !record->calls.is_empty(); }));
    harness.settle();
    EXPECT_EQ(record->responses.size(), 1u);
    EXPECT_EQ(record->calls, (Vector<StringView> { "process response end of body"sv }));
    size_t end_count = 0;
    for (auto const& step : commands->steps)
        end_count += step == "end of fetch 1"sv;
    EXPECT_EQ(end_count, 1u);
    EXPECT(!is_rooted(token));
}

// A sync XHR whose body is whole at its handover reads it at once, and the agent posts the stop of its request to the
// fetch engine, which stops the request and ends the fetch.
TEST_CASE(a_sync_xhr_whose_body_is_whole_at_its_handover_posts_the_stop_of_its_request)
{
    auto const body = "the whole body"sv;
    auto const path = ByteString::formatted("{}/TestFetchAlgorithms-{:016x}", Core::StandardPaths::tempfile_directory(), get_random<u64>());
    {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew));
        MUST(file->write_until_depleted(body.bytes()));
    }
    // A file that is still mapped cannot be removed on Windows, so this runs last, once the request has unmapped it.
    ScopeGuard remove_file = [&path] { MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed)); };

    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    // Once the XHR's request has started, its headers and its whole body arrive together, before its response is handed
    // over, and the request is left unfinished. Ten seconds after the XHR starts, they arrive regardless.
    struct {
        bool answered { false };
        size_t ticks { 0 };
    } seen;
    auto timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [&] {
        if (seen.answered)
            return;
        if (!request_server.messages().contains_slow("start 0 GET /whole-body-sync-xhr"sv) && ++seen.ticks < 10'000)
            return;
        seen.answered = true;
        request_server.send_headers(0, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
        request_server.send_body_file(0, path, body.length());
        request_server.deliver_sent_messages();
    });
    timer->start();
    auto result = run_script(harness, "const xhr = new XMLHttpRequest(); xhr.open('GET', 'http://127.0.0.1:8000/whole-body-sync-xhr', false); xhr.send(); `${xhr.status} ${xhr.responseText}`;"sv);
    timer->stop();
    request_server.wait_for_message("stop 0"sv);
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT(result.is_string());
    if (result.is_string())
        EXPECT_EQ(result.as_string().utf16_string(), "200 the whole body"_utf16);
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /whole-body-sync-xhr", "stop 0" }));
    EXPECT_EQ(commands->steps.size(), 3u);
    if (commands->steps.size() != 3)
        return;
    EXPECT_EQ(commands->steps[0], "start fetch 1"sv);
    EXPECT(commands->steps[1].starts_with("stop transport of request 0 of epoch "sv));
    EXPECT_EQ(commands->steps[2], "end of fetch 1"sv);
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
static Vector<TransportActivity> each_network_activity(u64 request_id)
{
    Vector<TransportActivity> activities;
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = TransportActivity::RequestStarted {
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
        .activity = TransportActivity::ResponseHeaders {
            .status_code = 200,
            .reason_phrase = "OK"_string,
            .headers = { { "Content-Type"sv, "text/plain"sv } },
            .cache_state = Requests::CacheState::Local,
        },
    });
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = TransportActivity::ResponseBodyBytes { Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("world"sv.bytes()))) },
    });
    activities.append({
        .request_id = Web::Fetch::Engine::RequestId { request_id },
        .activity = TransportActivity::RequestFinished {
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
    auto started = harness.record_fetches_started_on_the_agents_executor();
    auto request = harness.create_request();
    (void)harness.fetch(request, FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard });
    harness.run_tasks_until(GC::create_function(heap, GC::captures(request), [](auto& request) { return request->done(); }));
    VERIFY(started->fetch_params.size() == 1);
    auto fetch_params = started->fetch_params.take_first();

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
    harness.run_until_responses(record, 1);

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

    auto steps = make_ref_counted<EndOfBodySteps>();
    controller->when_body_ends(Web::Fetch::Engine::DeliveryId(1), GC::create_function(harness.realm->heap(), GC::captures(steps, agent_timing_info), [](auto& steps, auto& agent_timing_info, double end_time) {
        steps->end_times.append(end_time);
        steps->had_network_phases = agent_timing_info->final_connection_timing_info().has_value();
    }));

    event_sink.handle_event(RequestTransmissionEnded { network_phases(10) });
    EXPECT(steps->end_times.is_empty());
    EXPECT_EQ(agent_timing_info->final_network_request_start_time(), 10);

    EndOfBody end_of_body {
        .delivery_id = Web::Fetch::Engine::DeliveryId(1),
        .end_time = 30,
    };
    event_sink.handle_event(move(end_of_body));
    EXPECT_EQ(steps->end_times, (Vector<double> { 30 }));
    EXPECT(steps->had_network_phases);
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

TEST_CASE(a_redirect_of_a_request_whose_redirect_mode_is_error_reports_a_fetch_error_to_webdriver_bidi)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    StandInRequestServer request_server { harness };

    auto events = make_ref_counted<ReportedNetworkEvents>();
    WebDriverBiDiSession session {
        { "network.fetchError"_string },
        [events](Web::HTML::Navigable const&, String const& name, JsonValue) { events->names.append(name); },
    };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_request_for("http://127.0.0.1:8000/redirects"sv);
    request->set_mode(Request::Mode::CORS);
    request->set_redirect_mode(Request::RedirectMode::Error);
    (void)harness.fetch(request, recording_responses(harness.realm->heap(), record));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 302, { { "Location"sv, "/final"sv }, { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_finished(request_id);
    harness.run_until_responses(record, 1);
    harness.settle();

    EXPECT(record->responses[0]->is_network_error());
    EXPECT_EQ(events->names, (Vector<String> { "network.fetchError"_string }));
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
    harness.run_until_responses(record, 1);
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
    auto started = harness.record_started_fetches();
    StandInRequestServer request_server { harness };
    WebDriverBiDiSession session { { "network.responseCompleted"_string }, [](Web::HTML::Navigable const&, String const&, JsonValue) { } };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_navigation_request_for("http://127.0.0.1:8000/manual-redirect"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto& control_block = controller->control_block();

    // WebDriver BiDi completes the redirect, which sets the fetch's end time before the redirect is handed over.
    auto redirect_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(redirect_id, 302, { { "Location"sv, "/final"sv } });
    harness.run_until_responses(record, 1);
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
    harness.run_until_responses(record, 2);
    run_until_encoded_size(harness, record->responses[1]->record()->unsafe_response(), 1234);

    // The final response's timing info takes the fetch's end time, and the redirect and network fields of its own hop.
    auto final_timing_info = control_block.agent_timing_info();
    VERIFY(final_timing_info);
    EXPECT_NE(final_timing_info.ptr(), redirect_timing_info.ptr());
    EXPECT_EQ(final_timing_info->end_time(), redirect_end_time);
    VERIFY(started->fetch_params.size() == 1);
    auto const& engine_timing_info = *started->fetch_params[0]->timing_info();
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
    harness.run_until_responses(record, 1);

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
    // A traversal back to a file: entry, which fetches it with the client of the document it replaces.
    auto request = harness.create_navigation_request_for(url->serialize());
    request->set_history_navigation(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record), document));
    harness.run_until_responses(record, 1);
    auto response = record->responses[0]->record()->unsafe_response();
    run_until_encoded_size(harness, response, 11);

    // A load without a RequestServer request has its network phases at its post-redirect start time.
    auto full_timing_info = controller->full_timing_info();
    VERIFY(full_timing_info);
    EXPECT_EQ(full_timing_info.ptr(), controller->control_block().agent_timing_info().ptr());
    expect_network_phases_of(*full_timing_info, {});
}

// A load without a RequestServer request runs its callbacks after a stop of its fetch, so main fetch still hands its
// response over, and the fetch ends there.
TEST_CASE(a_fetch_without_a_request_server_request_stopped_before_its_response_ends_at_its_handover)
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
    auto commands = harness.record_commands();
    ResourceLoaderWithoutRequestServer resource_loader { harness.realm->heap() };
    auto page_client = NetworkActivityPageClient::create(harness.realm->heap(), Web::PageId { 1 });
    auto document = create_document_of_page(page_client->page());

    auto record = make_ref_counted<AlgorithmCalls>();
    auto url = URL::create_with_file_scheme(path);
    VERIFY(url.has_value());
    // A traversal back to a file: entry, as above.
    auto request = harness.create_navigation_request_for(url->serialize());
    request->set_history_navigation(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record), document));
    controller->stop_fetch();
    harness.run_until_step(commands, "end of fetch 1"sv);

    // The stop replaced the fetch's algorithms, so the handover runs none of them.
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "stop 1", "end of fetch 1" }));
    EXPECT(controller->control_block().agent_timing_info());
    EXPECT(record->responses.is_empty());
    EXPECT(!is_rooted(controller->token()));
}

// The tasks of the document no longer run once its navigable is destroyed, so they stay queued until it is destroyed too.
static void hold_the_tasks_of(Web::DOM::Document& document)
{
    document.navigable()->set_has_been_destroyed();
}

TEST_CASE(a_fetch_stopped_while_main_fetch_reads_its_body_hands_its_failure_over)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "http://127.0.0.1:8000/stopped-mid-read"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(controller), [](auto& controller) { return controller->response_arrived(); }));

    harness.with_execution_context([&] { controller->stop_fetch(); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    request_server.wait_for_message("stop 0"sv);
    harness.settle();

    // The stop replaced the fetch's algorithms, so the handover runs none of them.
    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "stop 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /stopped-mid-read", "stop 0" }));
    EXPECT(was_handed_over(*controller));
    EXPECT(record->responses.is_empty());
    EXPECT(!is_rooted(controller->token()));
}

// A body that is already in memory is read, and its response handed over, without a task of the document.
TEST_CASE(a_fetch_whose_document_holds_its_tasks_reads_a_body_in_memory_and_hands_it_over)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    auto document = create_test_document();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "data:,"sv);
    hold_the_tasks_of(*document);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record), document));
    harness.run_until_step(commands, "end of fetch 1"sv);
    EXPECT(was_handed_over(*controller));

    // The response's tasks go with the document.
    harness.with_execution_context([&] { document->destroy(); });
    harness.run_until_step(commands, "stop 1"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of fetch 1", "stop 1" }));
    EXPECT(record->responses.is_empty());
    EXPECT(!is_rooted(controller->token()));
}

// The abort stops the request while main fetch reads the body to check its integrity, and main fetch hands a network
// error over.
TEST_CASE(a_fetch_aborted_while_main_fetch_reads_its_body_hands_over_a_network_error)
{
    auto const body = "the start of the body"sv;
    auto const path = ByteString::formatted("{}/TestFetchAlgorithms-{:016x}", Core::StandardPaths::tempfile_directory(), get_random<u64>());
    {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew));
        MUST(file->write_until_depleted(body.bytes()));
    }
    // A file that is still mapped cannot be removed on Windows, so this runs last, once the request has unmapped it.
    ScopeGuard remove_file = [&path] { MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed)); };

    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "http://127.0.0.1:8000/aborted-mid-read"sv);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record)));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_body_file(request_id, path, body.length());
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(controller), [](auto& controller) { return controller->response_arrived(); }));

    harness.with_execution_context([&] { controller->abort(*harness.realm, {}); });
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.run_until_responses(record, 1);
    request_server.wait_for_message("stop 0"sv);

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "abort 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /aborted-mid-read", "stop 0" }));
    EXPECT(record->responses[0]->is_network_error());
    EXPECT(!is_rooted(controller->token()));
}

// A keepalive fetch outlives its document, and main fetch reads the whole of its body, past the channel's high watermark,
// and hands the response over.
TEST_CASE(a_keepalive_fetch_whose_document_is_destroyed_while_main_fetch_reads_its_body_reads_all_of_it)
{
    auto const body = ByteBuffer::create_zeroed(2 * MiB).release_value();
    auto const path = ByteString::formatted("{}/TestFetchAlgorithms-{:016x}", Core::StandardPaths::tempfile_directory(), get_random<u64>());
    {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew));
        MUST(file->write_until_depleted(body.bytes()));
    }
    // A file that is still mapped cannot be removed on Windows, so this runs last, once the request has unmapped it.
    ScopeGuard remove_file = [&path] { MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed)); };

    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };
    auto document = create_test_document();

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = create_request_checked_for_integrity(harness, "http://127.0.0.1:8000/keepalive-mid-read"sv);
    // The digest of the body.
    request->set_integrity_metadata("sha256-VkfwXsGJWJR9ModO63iPo5agXQurfBtx8RLOt+mzHu4="_utf16);
    request->set_keepalive(true);
    auto controller = GC::make_root(harness.fetch(request, recording_responses(harness.realm->heap(), record), document));
    auto request_id = request_server.wait_for_request("GET"sv);
    request_server.send_headers(request_id, 200, { { "Access-Control-Allow-Origin"sv, "*"sv } });
    request_server.send_body_file(request_id, path, body.size());
    harness.run_tasks_until(GC::create_function(harness.realm->heap(), GC::captures(controller), [](auto& controller) { return controller->response_arrived(); }));

    // The destruction leaves the keepalive fetch running.
    harness.with_execution_context([&] { document->destroy(); });
    request_server.send_finished(request_id, finished_timing_info(1000));
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of fetch 1" }));
    EXPECT_EQ(request_server.messages(), (Vector<ByteString> { "start 0 GET /keepalive-mid-read" }));
    VERIFY(harness.handed_over->responses.size() == 1);
    auto const& response = *harness.handed_over->responses[0];
    VERIFY(!response.is_network_error());
    EXPECT_EQ(response.body()->length(), body.size());
    EXPECT(!is_rooted(controller->token()));
}

// A request with a transfer lease runs the callbacks for what arrives with its finish, after its load has ended.
TEST_CASE(what_a_leased_request_delivers_after_its_load_has_ended_reaches_no_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;
    auto commands = harness.record_commands();
    StandInRequestServer request_server { harness };

    auto record = make_ref_counted<AlgorithmCalls>();
    auto request = harness.create_navigation_request_for("http://127.0.0.1:8000/leased"sv);
    request->set_method("POST");
    request->set_body(MUST(ByteBuffer::copy("hello"sv.bytes())));
    auto token = harness.fetch_as_a_navigation(request, recording_responses(harness.realm->heap(), record))->token();
    auto request_id = request_server.wait_for_request("POST"sv);
    request_server.send_headers(request_id, 200);
    harness.run_until_responses(record, 1);
    request_server.send_finished(request_id);
    request_server.send_headers(request_id, 200);
    request_server.send(MUST(Messages::RequestClient::RequestUploadProgress::static_encode(request_id, 5)));
    harness.run_until_step(commands, "end of fetch 1"sv);
    harness.settle();

    EXPECT_EQ(commands->steps, (Vector<ByteString> { "start fetch 1", "end of body of 1", "end of fetch 1" }));
    EXPECT(!is_rooted(token));
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
