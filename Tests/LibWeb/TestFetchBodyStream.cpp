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
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Fetching/BodyStreamPullSource.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>

using Web::Fetch::Engine::FetchByteChannel;
using Web::Fetch::Engine::FetchByteChannelProducerSink;
using Web::Fetch::Infrastructure::FetchAlgorithms;
using Web::Fetch::Infrastructure::FetchParams;

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

GC::Ref<FetchParams> create_sync_xhr_fetch_params()
{
    auto algorithms = FetchAlgorithms::create(FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::SyncAccumulate });
    auto request = Web::Fetch::Infrastructure::Request::create();
    auto timing_info = Web::Fetch::Infrastructure::FetchTimingInfo::create();
    return FetchParams::create(move(request), algorithms, move(timing_info), Web::current_loader_config());
}

struct BodyStreamHarness {
    BodyStreamHarness()
        : realm(create_test_document()->relevant_settings_object().realm())
        , execution_context(*realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes)
    {
    }

    void create_stream()
    {
        channel = FetchByteChannel::create({ .low = 16, .high = 64 }, adopt_ref(*new IgnoringProducerSink));
        stream = Web::Fetch::Fetching::create_body_stream_from_channel(*realm, fetch_params, *channel);
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
    GC::Ptr<FetchParams const> fetch_params;
    RefPtr<FetchByteChannel> channel;
    GC::Ptr<Web::Streams::ReadableStream> stream;
    GC::Ptr<Web::Streams::ReadableStreamDefaultReader> reader;
};

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
    harness.fetch_params = create_sync_xhr_fetch_params();
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
