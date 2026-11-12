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
#include <LibWeb/Fetch/Engine/EngineEvent.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/FetchEventSink.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebIDL/DOMException.h>

using Web::Fetch::Engine::Event::ReadRequestBodyStream;
using Web::Fetch::Engine::Event::RequestBodyEndOfBody;
using Web::Fetch::Engine::Event::RequestBodyProgress;
using Web::Fetch::Engine::Event::RequestBodySentFromSource;
using Web::Fetch::Fetching::FetchEventSink;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchController;
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
        Web::HTML::surrounding_agent().fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor();
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

    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch(Request& request, FetchAlgorithms::Input input) const
    {
        return fetch_with(request, FetchAlgorithms::create(move(input)));
    }

    GC::Ref<Web::Fetch::Infrastructure::FetchController> fetch_with(Request& request, FetchAlgorithms const& algorithms) const
    {
        Web::HTML::TemporaryExecutionContext execution_context { *realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        return Web::Fetch::Fetching::fetch(*realm, request, algorithms);
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
