/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>

using Web::Fetch::Engine::FetchBodyDeliveryGate;
using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::FetchByteChannelConsumerSink;
using Web::Fetch::Engine::FetchByteChannelProducerSink;
using Web::Fetch::Infrastructure::Body;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchParams;
using Web::Fetch::Infrastructure::MAX_SNIFF_BYTES;

namespace {

class IgnoringProducerSink final : public FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64) override { }
    virtual void consumer_cancelled() override { }
};

// What one read of a stream gives.
class RecordingReadRequest final : public Web::Streams::ReadRequest {
    GC_CELL(RecordingReadRequest, Web::Streams::ReadRequest);
    GC_DECLARE_ALLOCATOR(RecordingReadRequest);

public:
    virtual void on_chunk(JS::Value chunk) override
    {
        auto& array = as<JS::Uint8Array>(chunk.as_object());
        auto record = JS::make_typed_array_with_buffer_witness_record(array, JS::ArrayBuffer::Order::SeqCst);
        m_chunk = MUST(array.viewed_array_buffer()->copy_to_byte_buffer(array.byte_offset(), JS::typed_array_byte_length(record)));
        m_chunk_received = true;
    }
    virtual void on_close() override { m_closed = true; }
    virtual void on_error(JS::Value) override { m_errored = true; }

    bool settled() const { return m_chunk_received || m_closed || m_errored; }
    StringView chunk() const { return m_chunk_received ? StringView { m_chunk } : "<no chunk>"sv; }
    bool closed() const { return m_closed; }

private:
    ByteBuffer m_chunk;
    bool m_chunk_received { false };
    bool m_closed { false };
    bool m_errored { false };
};

GC_DEFINE_ALLOCATOR(RecordingReadRequest);

NonnullRefPtr<FetchParams> create_sync_xhr_fetch_params(Web::Fetch::Infrastructure::FetchController& controller)
{
    auto algorithms = FetchAlgorithms::create(FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::SyncAccumulate });
    auto request = Web::Fetch::Infrastructure::Request::create();
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    return FetchParams::create(controller, move(request), algorithms, move(timing_info), Web::current_loader_config());
}

struct BodyStreamHarness {
    BodyStreamHarness()
        : realm(create_test_document()->relevant_settings_object().realm())
        , execution_context(*realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes)
    {
    }

    void create_stream(FetchByteChannel::Watermarks watermarks = { .low = 16, .high = 64 })
    {
        channel = FetchByteChannel::create(watermarks, adopt_ref(*new IgnoringProducerSink));
        body = Web::Fetch::Fetching::create_body_from_channel(*realm, fetch_controller, *channel);
        stream = body->stream();
    }

    // The bytes the channel holds that have not been pulled into the stream.
    u64 channel_byte_count()
    {
        return channel->peek_up_to(0).buffered_byte_count;
    }

    // Reads one chunk of the stream, running the event loop until the read settles, or for ten seconds.
    GC::Ref<RecordingReadRequest> read()
    {
        if (!reader)
            reader = MUST(stream->get_a_reader());
        auto request = realm->heap().allocate<RecordingReadRequest>();
        reader->read_a_chunk(*request);
        if (!request->settled()) {
            auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [] { });
            deadline->start();
            Web::HTML::main_thread_event_loop().spin_until(GC::create_function(realm->heap(), [request, deadline] {
                return request->settled() || !deadline->is_active();
            }));
        }
        return request;
    }

    // Runs every deferred invocation queued so far, which includes every channel wake.
    static void run_wakes()
    {
        for (size_t i = 0; i < 3; ++i)
            Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    GC::Ref<JS::Realm> realm;
    Web::HTML::TemporaryExecutionContext execution_context;
    RefPtr<FetchParams const> fetch_params;
    GC::Ptr<Web::Fetch::Infrastructure::FetchController> fetch_controller;
    RefPtr<FetchByteChannel> channel;
    GC::Ptr<Body> body;
    GC::Ptr<Web::Streams::ReadableStream> stream;
    GC::Ptr<Web::Streams::ReadableStreamDefaultReader> reader;
};

struct SniffWaiter {
    size_t wake_count { 0 };
    ByteBuffer bytes;
    Optional<Body::SniffOutcome> outcome;

    Body::SniffBytesCallback callback(GC::Heap& heap)
    {
        return GC::create_function(heap, [this](Body::SniffBytes sniff_bytes) {
            ++wake_count;
            bytes = MUST(ByteBuffer::copy(sniff_bytes.bytes));
            outcome = sniff_bytes.outcome;
        });
    }
};

Core::ImmutableBytes make_bytes(size_t size, u8 first_value)
{
    auto bytes = MUST(ByteBuffer::create_uninitialized(size));
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<u8>(first_value + i);
    return Core::ImmutableBytes::adopt(move(bytes));
}

// Runs channel wakes and networking tasks until the waiter has woken, or for ten seconds.
void wait_for_wake(GC::Heap& heap, SniffWaiter const& waiter)
{
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [] { });
    deadline->start();
    Web::HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&waiter, deadline] {
        return waiter.wake_count > 0 || !deadline->is_active();
    }));
}

// Runs channel wakes and every networking task they queue.
void run_wakes_and_tasks(GC::Heap& heap)
{
    BodyStreamHarness::run_wakes();
    IGNORE_USE_IN_ESCAPING_LAMBDA bool reached = false;
    Web::HTML::queue_a_task(Web::HTML::Task::Source::Networking, Web::HTML::main_thread_event_loop(), nullptr, GC::create_function(heap, [&reached] {
        reached = true;
    }));
    Web::HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&reached] { return reached; }));
}

}

TEST_CASE(an_error_reaches_a_stream_nobody_reads)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream();

    (void)harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) });
    EXPECT(harness.stream->is_readable());

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->is_errored());
}

TEST_CASE(an_empty_close_closes_a_stream_nobody_reads)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream();

    (void)harness.channel->close();
    EXPECT(harness.stream->is_readable());

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->is_closed());
}

TEST_CASE(a_close_leaves_unread_bytes_readable)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream();

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("unread"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->close();

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->is_readable());
    EXPECT_EQ(harness.channel_byte_count(), 6u);

    auto read = harness.read();
    EXPECT_EQ(read->chunk(), "unread"sv);
    EXPECT(harness.stream->is_closed());
}

TEST_CASE(bytes_reach_a_stream_while_the_event_loop_is_paused)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& controller_registry = static_cast<Web::HTML::Agent&>(*harness.realm->vm().agent()).fetch_controller_registry;
    harness.fetch_controller = Web::Fetch::Infrastructure::FetchController::create(controller_registry);
    harness.fetch_params = create_sync_xhr_fetch_params(*harness.fetch_controller);
    auto pause = Web::HTML::main_thread_event_loop().pause(Web::HTML::EventLoop::UpdateTheRendering::No);
    harness.create_stream();

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("pushed"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    BodyStreamHarness::run_wakes();
    EXPECT_EQ(harness.channel_byte_count(), 0u);

    (void)harness.channel->close();
    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->is_readable());

    auto read = harness.read();
    EXPECT_EQ(read->chunk(), "pushed"sv);
    EXPECT(harness.stream->is_closed());
}

TEST_CASE(sniffing_waits_for_the_resource_header_and_leaves_it_in_the_channel)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    auto whole_body = make_bytes(2000, 0);
    EXPECT_EQ(harness.channel->write(whole_body.slice(0, 500)), FetchByteChannel::WriteResult::Accepted);
    run_wakes_and_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 0u);

    EXPECT_EQ(harness.channel->write(whole_body.slice(500, 1500)), FetchByteChannel::WriteResult::Accepted);
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes.bytes(), whole_body.bytes().trim(MAX_SNIFF_BYTES));

    // A transfer of the body after sniffing still carries it from its first byte.
    ByteBuffer transferred;
    for (auto const& taken : harness.channel->transfer_out())
        transferred.append(taken.bytes());
    EXPECT_EQ(transferred.bytes(), whole_body.bytes());
}

TEST_CASE(sniffing_a_body_set_up_during_a_pause_waits_for_the_resource_header)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& heap = harness.realm->heap();
    auto whole_body = make_bytes(2000, 0);
    {
        auto pause = Web::HTML::main_thread_event_loop().pause(Web::HTML::EventLoop::UpdateTheRendering::No);
        harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
        EXPECT_EQ(harness.channel->write(whole_body.slice(0, 100)), FetchByteChannel::WriteResult::Accepted);
        BodyStreamHarness::run_wakes();
    }

    EXPECT_EQ(harness.channel->write(whole_body.slice(100, 400)), FetchByteChannel::WriteResult::Accepted);
    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));
    run_wakes_and_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 0u);

    EXPECT_EQ(harness.channel->write(whole_body.slice(500, 1500)), FetchByteChannel::WriteResult::Accepted);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes.bytes(), whole_body.bytes().trim(MAX_SNIFF_BYTES));

    ByteBuffer transferred;
    for (auto const& taken : harness.channel->transfer_out())
        transferred.append(taken.bytes());
    EXPECT_EQ(transferred.bytes(), whole_body.bytes());
}

TEST_CASE(sniffing_a_body_that_ends_short_gives_all_of_it)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("short"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->close();
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes, MUST(ByteBuffer::copy("short"sv.bytes())));
}

TEST_CASE(sniffing_a_body_that_errors_fails)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    EXPECT_EQ(harness.channel->write(MUST(Core::ImmutableBytes::copy("partial"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) });
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Failed);
}

TEST_CASE(sniffing_keeps_a_complete_header_through_a_later_error)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));

    // The body fails once its resource header is complete, before the sniff waiter's wake has run.
    auto header = make_bytes(MAX_SNIFF_BYTES, 0);
    EXPECT_EQ(harness.channel->write(header), FetchByteChannel::WriteResult::Accepted);
    (void)harness.channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) });
    EXPECT_EQ(waiter.wake_count, 0u);
    wait_for_wake(heap, waiter);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Complete);
    EXPECT_EQ(waiter.bytes.bytes(), header.bytes());

    BodyStreamHarness::run_wakes();
    EXPECT(harness.stream->is_errored());
}

// A transport's teardown cancels or transfers the channel itself, before the resource header is complete or after the
// channel kept it, and the sniff fails once either way.
static void check_a_channel_ending_fails_the_sniff_once(size_t written_byte_count, bool transfer)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    harness.create_stream({ .low = 2 * KiB, .high = 4 * KiB });
    auto& heap = harness.realm->heap();

    SniffWaiter waiter;
    harness.body->wait_for_sniff_bytes(waiter.callback(heap));
    EXPECT_EQ(harness.channel->write(make_bytes(written_byte_count, 0)), FetchByteChannel::WriteResult::Accepted);
    if (transfer)
        (void)harness.channel->transfer_out();
    else
        (void)harness.channel->cancel();

    wait_for_wake(heap, waiter);
    run_wakes_and_tasks(heap);
    EXPECT_EQ(waiter.wake_count, 1u);
    EXPECT(waiter.outcome == Body::SniffOutcome::Failed);
}

TEST_CASE(a_cancelled_channel_fails_a_sniff_short_of_its_header)
{
    check_a_channel_ending_fails_the_sniff_once(100, false);
}

TEST_CASE(a_cancelled_channel_fails_a_sniff_whose_header_it_kept)
{
    check_a_channel_ending_fails_the_sniff_once(MAX_SNIFF_BYTES, false);
}

TEST_CASE(a_transferred_channel_fails_a_sniff_short_of_its_header)
{
    check_a_channel_ending_fails_the_sniff_once(100, true);
}

TEST_CASE(a_transferred_channel_fails_a_sniff_whose_header_it_kept)
{
    check_a_channel_ending_fails_the_sniff_once(MAX_SNIFF_BYTES, true);
}

TEST_CASE(a_body_over_bytes_sniffs_at_once)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& heap = harness.realm->heap();

    auto bytes = make_bytes(2000, 0);
    auto body = Body::create_over_bytes(*harness.realm, bytes);
    auto sniff_bytes = body->sniff_bytes_if_available();
    EXPECT(sniff_bytes.has_value());
    EXPECT_EQ(sniff_bytes->bytes, bytes.bytes().trim(MAX_SNIFF_BYTES));

    SniffWaiter waiter;
    body->wait_for_sniff_bytes(waiter.callback(heap));
    EXPECT_EQ(waiter.wake_count, 1u);
}

namespace {

class CountingConsumerSink final : public FetchByteChannelConsumerSink {
public:
    virtual void wake(Web::Fetch::Engine::WakeTicket) override { ++wake_count; }

    size_t wake_count { 0 };
};

}

TEST_CASE(stopping_a_whole_body_transport_cancels_its_channel_and_wakes_its_reader)
{
    Core::EventLoop event_loop;
    auto gate = FetchBodyDeliveryGate::create(FetchBodyDeliveryGate::NavigationHold::None);
    auto channel = FetchByteChannel::create({ .low = 16, .high = 64 }, gate->create_channel_sink());
    gate->grant_credit(channel->initial_credit());
    auto reader = adopt_ref(*new CountingConsumerSink);
    channel->attach_consumer(reader);
    (void)channel->arm_waiter(FetchByteChannel::EndOnly {});

    auto transport = Web::Fetch::Fetching::create_whole_body_transport(*gate, *channel);
    transport->attach_to(gate);
    transport->set_body(make_bytes(1000, 0), [&](Requests::ResponseData data) {
        EXPECT_EQ(gate->did_deliver(data.bytes().size()), Web::Fetch::Engine::FetchBodyDeliveryGate::DeliveryResult::Accepted);
        (void)channel->write(MUST(Core::ImmutableBytes::copy(data.bytes())));
    });
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(channel->peek_up_to(0).buffered_byte_count, 64u);
    EXPECT_EQ(reader->wake_count, 0u);

    transport->stop();
    EXPECT(channel->peek_up_to(0).state.has<FetchByteChannel::ConsumerCancelled>());
    EXPECT_EQ(reader->wake_count, 1u);
}

TEST_CASE(network_completion_runs_only_the_steps_awaited_for_its_body)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    BodyStreamHarness harness;
    auto& heap = harness.realm->heap();
    auto& controller_registry = static_cast<Web::HTML::Agent&>(*harness.realm->vm().agent()).fetch_controller_registry;
    auto controller = Web::Fetch::Infrastructure::FetchController::create(controller_registry);
    auto earlier = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink));
    auto later = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink));

    // A later body's steps replace an earlier body's, and run once.
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t earlier_runs = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t later_runs = 0;
    controller->when_network_completes_body(*earlier, GC::create_function(heap, [&] { ++earlier_runs; }));
    controller->when_network_completes_body(*later, GC::create_function(heap, [&] { ++later_runs; }));
    controller->did_complete_network_body(*earlier);
    EXPECT_EQ(earlier_runs, 0u);
    EXPECT_EQ(later_runs, 0u);
    controller->did_complete_network_body(*later);
    controller->did_complete_network_body(*later);
    EXPECT_EQ(later_runs, 1u);

    // A stopped fetch runs no awaited steps.
    controller->when_network_completes_body(*earlier, GC::create_function(heap, [&] { ++earlier_runs; }));
    controller->stop_fetch();
    controller->did_complete_network_body(*earlier);
    EXPECT_EQ(earlier_runs, 0u);
}
