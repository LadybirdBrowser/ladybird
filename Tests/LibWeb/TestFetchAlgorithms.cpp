/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/RootVector.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::Request;
using Web::Fetch::Infrastructure::ResponseView;

namespace {

struct FetchHarness {
    FetchHarness()
        : document(create_test_document())
        , realm(document->relevant_settings_object().realm())
    {
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
    void run_tasks_until(ESCAPING Function<bool()> condition) const
    {
        auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [] { });
        deadline->start();
        Web::HTML::main_thread_event_loop().spin_until(GC::create_function(realm->heap(), [condition = move(condition), deadline] {
            return condition() || !deadline->is_active();
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

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<StringView> calls;
    GC::RootVector<GC::Ref<ResponseView>> responses;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView> response) {
        calls.append("process response"sv);
        responses.append(response);
    };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView> response) {
        calls.append("process response end-of-body"sv);
        responses.append(response);
    };
    input.process_response_consume_body = [&](GC::Ref<ResponseView> response, FetchAlgorithms::BodyBytes body) {
        calls.append("process response consume body"sv);
        responses.append(response);
        EXPECT(body.has<Empty>());
    };

    auto request = harness.create_request();
    (void)harness.fetch(request, move(input));
    harness.run_tasks_until([&] { return calls.size() == 3; });

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

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<StringView> calls;
    GC::RootVector<GC::Ref<ResponseView>> responses;
    Optional<ByteBuffer> consumed_bytes;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView> response) {
        calls.append("process response"sv);
        responses.append(response);
    };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView> response) {
        calls.append("process response end-of-body"sv);
        responses.append(response);
    };
    input.process_response_consume_body = [&](GC::Ref<ResponseView> response, FetchAlgorithms::BodyBytes body) {
        calls.append("process response consume body"sv);
        responses.append(response);
        if (auto const* bytes = body.get_pointer<Core::ImmutableBytes>())
            consumed_bytes = MUST(ByteBuffer::copy(bytes->bytes()));
    };

    auto request = harness.create_request_for("data:text/plain,hello"sv);
    (void)harness.fetch(request, move(input));
    harness.run_tasks_until([&] { return calls.size() == 3; });

    EXPECT_EQ(calls, (Vector<StringView> { "process response"sv, "process response end-of-body"sv, "process response consume body"sv }));
    VERIFY(responses.size() == 3);
    EXPECT(!responses[0]->is_network_error());
    EXPECT_EQ(responses[1].ptr(), responses[0].ptr());
    EXPECT_EQ(responses[2].ptr(), responses[0].ptr());
    VERIFY(consumed_bytes.has_value());
    EXPECT_EQ(*consumed_bytes, MUST(ByteBuffer::copy("hello"sv.bytes())));
}

TEST_CASE(a_stopped_fetch_runs_no_algorithm_but_its_request_is_done)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    size_t call_count = 0;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView>) { ++call_count; };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView>) { ++call_count; };
    input.process_response_consume_body = [&](GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { ++call_count; };

    IGNORE_USE_IN_ESCAPING_LAMBDA auto request = harness.create_request();
    auto controller = harness.fetch(request, move(input));
    controller->stop_fetch();
    harness.run_tasks_until([&] { return request->done(); });

    EXPECT_EQ(call_count, 0u);
    EXPECT(request->done());
}

TEST_CASE(a_stop_leaves_the_algorithms_for_the_next_fetch)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    FetchHarness harness;

    IGNORE_USE_IN_ESCAPING_LAMBDA size_t call_count = 0;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response = [&](GC::Ref<ResponseView>) { ++call_count; };
    input.process_response_end_of_body = [&](GC::Ref<ResponseView>) { ++call_count; };
    input.process_response_consume_body = [&](GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { ++call_count; };
    auto algorithms = FetchAlgorithms::create(move(input));

    IGNORE_USE_IN_ESCAPING_LAMBDA auto stopped_request = harness.create_request();
    harness.fetch_with(stopped_request, algorithms)->stop_fetch();
    harness.run_tasks_until([&] { return stopped_request->done(); });
    EXPECT_EQ(call_count, 0u);

    // A stop empties the stopped fetch's algorithms, not the caller's.
    auto request = harness.create_request();
    (void)harness.fetch_with(request, algorithms);
    harness.run_tasks_until([&] { return call_count == 3; });
    EXPECT_EQ(call_count, 3u);
}

static NEVER_INLINE void start_fetch_without_holding_its_controller(FetchHarness const& harness, Request& request, FetchAlgorithms::Input input)
{
    (void)harness.fetch(request, move(input));
}

static NEVER_INLINE void run_a_fetch_whose_controller_nothing_holds()
{
    Core::EventLoop event_loop;
    install_font_plugin();
    IGNORE_USE_IN_ESCAPING_LAMBDA FetchHarness harness;

    IGNORE_USE_IN_ESCAPING_LAMBDA bool consumed_body = false;
    FetchAlgorithms::Input input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
    input.process_response_consume_body = [&](GC::Ref<ResponseView>, FetchAlgorithms::BodyBytes) { consumed_body = true; };

    // A worker's script fetch has no fetch record, so only its own steps hold its controller.
    auto request = harness.create_request();
    request->set_destination(Request::Destination::Worker);
    start_fetch_without_holding_its_controller(harness, request, move(input));
    GC::scrub_stack_for_testing();
    harness.run_tasks_until([&] {
        harness.realm->heap().collect_garbage();
        return consumed_body;
    });

    EXPECT(consumed_body);
}

TEST_CASE(a_fetch_whose_controller_nothing_holds_still_runs_its_algorithms)
{
    GC::scrub_stack_for_testing();
    run_a_fetch_whose_controller_nothing_holds();
}
