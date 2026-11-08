/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <AK/RefCounted.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

using Web::Fetch::Infrastructure::FetchAlgorithms;
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
