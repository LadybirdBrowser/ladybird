/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Platform/FontPlugin.h>

using Web::Fetch::Infrastructure::Body;
using Web::Fetch::Infrastructure::FetchBody;
using Web::Fetch::Infrastructure::MAX_SNIFF_BYTES;

namespace {

struct SniffWaiter {
    size_t wake_count { 0 };
    ByteBuffer bytes;
    Optional<FetchBody::SniffOutcome> outcome;

    GC::Ref<GC::Function<void(FetchBody::SniffBytes)>> callback(GC::Heap& heap)
    {
        return GC::create_function(heap, [this](FetchBody::SniffBytes sniff_bytes) {
            ++wake_count;
            bytes = MUST(ByteBuffer::copy(sniff_bytes.bytes));
            outcome = sniff_bytes.outcome;
        });
    }
};

GC::Ref<Body> create_body_without_source(JS::Realm& realm)
{
    auto stream = Web::Fetch::Infrastructure::byte_sequence_as_body(realm, {})->stream();
    return Body::create(stream);
}

void install_font_plugin()
{
    static Web::Platform::FontPlugin font_plugin { false };
    static bool installed = false;
    if (installed)
        return;
    Web::Platform::FontPlugin::install(font_plugin);
    installed = true;
}

void spin_until(GC::Heap& heap, Function<bool()> condition)
{
    Web::HTML::main_thread_event_loop().spin_until(GC::create_function(heap, move(condition)));
}

// Runs every networking task queued so far.
void run_queued_tasks(GC::Heap& heap)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA bool reached = false;
    Web::HTML::queue_a_task(Web::HTML::Task::Source::Networking, Web::HTML::main_thread_event_loop(), nullptr, GC::create_function(heap, [&reached] {
        reached = true;
    }));
    spin_until(heap, [&reached] { return reached; });
}

}

TEST_CASE(sniff_waiter_runs_from_a_task_after_its_bytes_arrive)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& heap = realm->heap();
    auto body = create_body_without_source(*realm);

    SniffWaiter waiter;
    body->wait_for_sniff_bytes(waiter.callback(heap));

    auto bytes = MUST(ByteBuffer::create_uninitialized(MAX_SNIFF_BYTES));
    bytes.bytes().fill('a');
    body->append_sniff_bytes(bytes);
    EXPECT_EQ(waiter.wake_count, 0u);

    spin_until(heap, [&waiter] { return waiter.wake_count > 0; });
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT_EQ(waiter.bytes, bytes);
    EXPECT(waiter.outcome == FetchBody::SniffOutcome::Complete);
}

TEST_CASE(sniff_waiter_runs_once_however_often_its_bytes_are_completed)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& heap = realm->heap();
    auto body = create_body_without_source(*realm);

    SniffWaiter waiter;
    body->wait_for_sniff_bytes(waiter.callback(heap));

    body->append_sniff_bytes("short"sv.bytes());
    body->set_sniff_bytes_complete();
    body->set_sniff_bytes_complete();
    body->append_sniff_bytes("ignored"sv.bytes());
    EXPECT_EQ(waiter.wake_count, 0u);

    run_queued_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT_EQ(waiter.bytes, MUST(ByteBuffer::copy("short"sv.bytes())));
    EXPECT(waiter.outcome == FetchBody::SniffOutcome::Complete);
}

TEST_CASE(sniff_waiter_hears_a_failed_body_from_a_task)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& heap = realm->heap();
    auto body = create_body_without_source(*realm);

    SniffWaiter waiter;
    body->wait_for_sniff_bytes(waiter.callback(heap));

    body->append_sniff_bytes("partial"sv.bytes());
    body->set_sniff_bytes_failed();
    EXPECT_EQ(waiter.wake_count, 0u);

    run_queued_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == FetchBody::SniffOutcome::Failed);
}
