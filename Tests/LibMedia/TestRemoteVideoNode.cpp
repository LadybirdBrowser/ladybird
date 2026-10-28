/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ThreadSafeWeakable.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGfx/YUVData.h>
#include <LibIPC/Transport.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Producers/RemoteVideoProducer.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientConnection.h>
#include <LibMedia/VideoPresentation/VideoPresentationServerConnection.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include "TestMediaCommon.h"

TEST_SETUP
{
    Media::install_video_decoders({ Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ Media::audio_decoders() });
}

using namespace Media;

namespace {

constexpr auto TEST_CLIP = "big_buck_bunny_5s.webm"sv;

NonnullRefPtr<IncrementallyPopulatedStream> load_test_file(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
}

NonnullRefPtr<Demuxer> create_demuxer(NonnullRefPtr<IncrementallyPopulatedStream> const& stream)
{
    return MUST(Media::create_demuxer(stream));
}

NonnullRefPtr<DecodedVideoProducer> make_decoder(Core::EventLoop& loop, StringView path)
{
    auto stream = load_test_file(path);
    auto demuxer = create_demuxer(stream);
    auto tracks = MUST(demuxer->get_tracks_for_type(TrackType::Video));
    VERIFY(!tracks.is_empty());
    return MUST(DecodedVideoProducer::try_create(loop, demuxer, tracks[0]));
}

bool spin_until(Core::EventLoop& loop, Function<bool()> condition, AK::Duration timeout = AK::Duration::from_seconds(2))
{
    auto deadline = MonotonicTime::now_coarse() + timeout;
    while (MonotonicTime::now_coarse() < deadline) {
        if (condition())
            return true;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return condition();
}

Vector<AK::Duration> decode_reference_timestamps(StringView path)
{
    auto& loop = never_destroyed_event_loop();
    auto decoder = make_decoder(loop, path);
    decoder->start();

    Vector<AK::Duration> timestamps;
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(10);
    while (MonotonicTime::now_coarse() < deadline) {
        auto output = decoder->peek();
        if (output.status == PipelineStatus::HaveData) {
            timestamps.append(output.frame->timestamp());
            decoder->consume();
            continue;
        }
        if (output.status == PipelineStatus::EndOfStream)
            return timestamps;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    FAIL("Reference decode timed out");
    return timestamps;
}

// Bridges the two ends of the edge the way the IPC connection does. The sink's pump thread queues its messages, which
// the consumer's thread delivers in order, since RemoteVideoProducer is single-threaded; the consumer's delegates
// drive the sink directly.
struct VideoEdgeTestHarness
    : public AtomicRefCounted<VideoEdgeTestHarness>
    , public ThreadSafeWeakable<VideoEdgeTestHarness> {
    struct StatusMessage {
        PipelineStatus status { PipelineStatus::Pending };
        u32 seek_id { 0 };
    };
    using Message = Variant<RemoteVideoSink::LentFrame, StatusMessage>;
    struct PostedMessage {
        Message message;
        MonotonicTime post_time;
    };

    RefPtr<RemoteVideoSink> sink;
    RefPtr<RemoteVideoProducer> consumer;

    Mutex mutex;
    ConditionVariable signal { mutex };
    Vector<PostedMessage> pending_messages;
    bool signaled { false };
    // Messages are delivered only once they are this old, the way they take a while to cross between processes.
    AK::Duration delivery_delay;

    void post(Message message)
    {
        MutexLocker locker { mutex };
        pending_messages.append({ move(message), MonotonicTime::now() });
        signaled = true;
        signal.broadcast();
    }

    void wake()
    {
        MutexLocker locker { mutex };
        signaled = true;
        signal.broadcast();
    }

    void deliver_messages()
    {
        Vector<Message> messages;
        {
            MutexLocker locker { mutex };
            auto deliverable_count = 0uz;
            auto now = MonotonicTime::now();
            while (deliverable_count < pending_messages.size() && now - pending_messages[deliverable_count].post_time >= delivery_delay)
                deliverable_count++;
            for (size_t i = 0; i < deliverable_count; i++)
                messages.append(move(pending_messages[i].message));
            pending_messages.remove(0, deliverable_count);
        }
        for (auto& message : messages) {
            message.visit(
                [&](RemoteVideoSink::LentFrame& lent_frame) {
                    auto release = [weak = make_weak_ref(), lend_id = lent_frame.lend_id] {
                        if (auto harness = weak.strong_ref())
                            harness->sink->release_frame(lend_id);
                    };
                    auto frame = MUST(create_received_video_frame(lent_frame.metadata, move(lent_frame.pixels), 1, move(release)));
                    consumer->receive_frame(lent_frame.lend_id, lent_frame.seek_id, move(frame));
                },
                [&](StatusMessage const& status) {
                    consumer->receive_status(status.status, status.seek_id);
                });
        }
    }
};

void wire_full_node(VideoEdgeTestHarness& harness, NonnullRefPtr<VideoProducer> const& decoder)
{
    // The detached pump thread can invoke these delegates while the test scope is being torn down,
    // so they promote a weak reference to keep the harness alive for the duration of the call.
    RemoteVideoSink::Delegates sink_delegates;
    sink_delegates.send_frame = [weak = harness.make_weak_ref()](RemoteVideoSink::LentFrame lent_frame) {
        if (auto harness = weak.strong_ref())
            harness->post(move(lent_frame));
    };
    sink_delegates.send_status = [weak = harness.make_weak_ref()](PipelineStatus status, u32 seek_id) {
        if (auto harness = weak.strong_ref())
            harness->post(VideoEdgeTestHarness::StatusMessage { status, seek_id });
    };
    sink_delegates.notify_clock_resumed = [] { };
    sink_delegates.transmit_seek = [](InvalidateHeldData) { };
    sink_delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    harness.sink = MUST(RemoteVideoSink::create(move(sink_delegates)));

    RemoteVideoProducer::Delegates consumer_delegates;
    consumer_delegates.request_start = [&harness] { harness.sink->start_input(); };
    consumer_delegates.request_seek = [&harness](AK::Duration timestamp, InvalidateHeldData invalidate_held_data, u64 newest_received_lend_id) {
        harness.sink->seek_upstream(timestamp, invalidate_held_data, newest_received_lend_id);
    };
    harness.consumer = RemoteVideoProducer::create(move(consumer_delegates));
    harness.consumer->set_wake_handler([&harness] { harness.wake(); });

    // harness.consumer is set before connect_input, whose lock publishes it to the pump before the
    // pump can send anything to the consumer.
    MUST(harness.sink->connect_input(decoder));
    harness.consumer->start();
}

NonnullRefPtr<VideoFrame> create_pooled_frame(VideoFramePool& pool, AK::Duration timestamp, AK::Duration duration = AK::Duration::from_milliseconds(33))
{
    static constexpr Gfx::IntSize frame_size { 2, 2 };
    static constexpr u8 bit_depth = 8;
    auto subsampling = Subsampling(true, true);

    auto layout = MUST(frame_plane_layout(frame_size, bit_depth, subsampling));
    auto acquired = pool.try_acquire(layout.total_byte_count).release_value();
    return make_ref_counted<VideoFrame>(timestamp, duration, Gfx::Size<u32>(frame_size), bit_depth, subsampling, CodingIndependentCodePoints {}, move(acquired.storage));
}

// A frame as the consumer receives it, which counts its release.
NonnullRefPtr<VideoFrame> create_received_frame(VideoFramePool& pool, AK::Duration timestamp, Function<void()> on_release = [] { })
{
    auto frame = create_pooled_frame(pool, timestamp, AK::Duration::from_milliseconds(100));
    return MUST(create_received_video_frame(VideoFrameMetadata::for_frame(*frame), frame->pixel_buffer(), 1, move(on_release)));
}

RemoteVideoProducer::Delegates make_consumer_delegates(size_t& upstream_seeks)
{
    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [&upstream_seeks](AK::Duration, InvalidateHeldData, u64) { upstream_seeks++; };
    return delegates;
}

// Gives a consumer four frames at 1.0s..1.3s under seek 0, to exercise the seek fast path deterministically. Each frame
// runs the given callback, which must outlive them, once it is released.
void give_lookahead(RemoteVideoProducer& consumer, VideoFramePool& pool, Function<void()> const* on_release = nullptr)
{
    for (u64 i = 0; i < 4; i++) {
        consumer.receive_frame(i + 1, 0, create_received_frame(pool, AK::Duration::from_milliseconds(1000 + (i * 100)), [on_release] {
            if (on_release)
                (*on_release)();
        }));
    }
}

// Records what a sink lends, from its pump thread.
struct LentFrameRecorder : public AtomicRefCounted<LentFrameRecorder> {
    Mutex mutex;
    Vector<RemoteVideoSink::LentFrame> lent_frames;

    size_t count()
    {
        MutexLocker locker { mutex };
        return lent_frames.size();
    }

    RemoteVideoSink::LentFrame at(size_t index)
    {
        MutexLocker locker { mutex };
        return lent_frames[index];
    }
};

NonnullRefPtr<RemoteVideoSink> make_recorded_sink(LentFrameRecorder& recorder)
{
    RemoteVideoSink::Delegates delegates;
    delegates.send_frame = [recorder = NonnullRefPtr { recorder }](RemoteVideoSink::LentFrame lent_frame) {
        MutexLocker locker { recorder->mutex };
        recorder->lent_frames.append(move(lent_frame));
    };
    delegates.send_status = [](PipelineStatus, u32) { };
    delegates.notify_clock_resumed = [] { };
    delegates.transmit_seek = [](InvalidateHeldData) { };
    delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    return MUST(RemoteVideoSink::create(move(delegates)));
}

NonnullRefPtr<RemoteVideoSink> make_sink_with_inert_delegates()
{
    return make_recorded_sink(*make_ref_counted<LentFrameRecorder>());
}

}

// Without seeks the node must deliver every decoded frame, in order, exactly once: the count and
// timestamps are deterministic even though the pump and the drainer run concurrently.
TEST_CASE(remote_video_node_delivers_every_frame_in_order)
{
    auto reference = decode_reference_timestamps(TEST_CLIP);
    EXPECT(!reference.is_empty());

    auto& loop = never_destroyed_event_loop();
    auto decoder = make_decoder(loop, TEST_CLIP);
    auto harness = make_ref_counted<VideoEdgeTestHarness>();
    wire_full_node(*harness, decoder);

    Vector<AK::Duration> delivered;
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(10);
    while (MonotonicTime::now_coarse() < deadline) {
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        harness->deliver_messages();
        auto output = harness->consumer->peek();
        if (output.status == PipelineStatus::HaveData) {
            delivered.append(output.frame->timestamp());
            harness->consumer->consume();
            continue;
        }
        if (output.status == PipelineStatus::EndOfStream)
            break;
    }

    EXPECT_EQ(delivered, reference);
}

// Stress the boundary under genuine concurrency: the pump thread and the draining thread race while
// seeks are injected mid-drain. Seeks go backward, so a correct node visibly rewinds; a leaked
// superseded-generation frame comes from ahead of where we were and trips the assertion. Also catches
// crashes, deadlocks, and lend leaks (which would starve the pool and hang). Repeat the executable to
// widen the interleaving coverage.
TEST_CASE(remote_video_node_streams_and_seeks_under_concurrency)
{
    auto& loop = never_destroyed_event_loop();
    auto decoder = make_decoder(loop, TEST_CLIP);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto harness = make_ref_counted<VideoEdgeTestHarness>();
    wire_full_node(*harness, decoder);

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stop { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> frames_pulled { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stale_detected { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<i64> stale_observed_ms { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<i64> stale_pre_seek_ms { 0 };

    auto drainer = Threading::Thread::construct("Consumer Drainer"sv, [&]() -> intptr_t {
        auto const seek_step = AK::Duration::from_milliseconds(300);
        size_t since_seek = 0;
        bool have_consumed = false;
        AK::Duration last_consumed;
        bool just_seeked = false;
        AK::Duration pre_seek_timestamp;

        auto seek_to = [&](AK::Duration target) {
            pre_seek_timestamp = last_consumed;
            harness->consumer->seek(target);
            just_seeked = true;
            since_seek = 0;
        };

        while (!stop.load(AK::MemoryOrder::memory_order_relaxed)) {
            harness->deliver_messages();

            auto output = harness->consumer->peek();
            if (output.status == PipelineStatus::HaveData) {
                VERIFY(output.frame);
                auto timestamp = output.frame->timestamp();
                if (just_seeked) {
                    if (timestamp >= pre_seek_timestamp) {
                        stale_observed_ms.store(timestamp.to_milliseconds(), AK::MemoryOrder::memory_order_relaxed);
                        stale_pre_seek_ms.store(pre_seek_timestamp.to_milliseconds(), AK::MemoryOrder::memory_order_relaxed);
                        stale_detected.store(true, AK::MemoryOrder::memory_order_relaxed);
                    }
                    just_seeked = false;
                }

                if (have_consumed && ++since_seek >= 20) {
                    // Seek with this frame still queued, so the drop covers it as well as the frames behind it.
                    seek_to(last_consumed > seek_step ? last_consumed - seek_step : AK::Duration::zero());
                    continue;
                }

                last_consumed = timestamp;
                have_consumed = true;
                frames_pulled.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
                harness->consumer->consume();
                continue;
            }

            // Loop the clip (also a backward seek) so the stream keeps running for the test window.
            if (output.status == PipelineStatus::EndOfStream && have_consumed)
                seek_to(AK::Duration::zero());

            MutexLocker locker { harness->mutex };
            while (!harness->signaled && !stop.load(AK::MemoryOrder::memory_order_relaxed) && harness->pending_messages.is_empty())
                harness->signal.wait();
            harness->signaled = false;
        }
        return 0;
    });
    drainer->start();

    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(1);
    while (MonotonicTime::now_coarse() < deadline)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    stop.store(true, AK::MemoryOrder::memory_order_relaxed);
    harness->wake();
    (void)drainer->join();

    if (stale_detected.load(AK::MemoryOrder::memory_order_relaxed)) {
        FAIL(MUST(String::formatted("Stale frame at {}ms delivered after seeking back from {}ms",
            stale_observed_ms.load(AK::MemoryOrder::memory_order_relaxed),
            stale_pre_seek_ms.load(AK::MemoryOrder::memory_order_relaxed))));
    }
    EXPECT(frames_pulled.load(AK::MemoryOrder::memory_order_relaxed) > 0);
}

// Seeks in both directions, many of them landing in frames that are still on their way to the consumer, which the sink
// has to lend again. Whichever way a seek is resolved, the first frame after it must hold the target or come just
// before it, and the frames after that must follow in order, or a frame was skipped or a stale one delivered.
TEST_CASE(remote_video_node_resolves_seeks_into_frames_on_their_way)
{
    auto& loop = never_destroyed_event_loop();
    auto decoder = make_decoder(loop, TEST_CLIP);
    IGNORE_USE_IN_ESCAPING_LAMBDA auto harness = make_ref_counted<VideoEdgeTestHarness>();
    harness->delivery_delay = AK::Duration::from_milliseconds(3);
    wire_full_node(*harness, decoder);

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stop { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> seek_count { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> failure_detected { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<i64> failure_target_ms { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<i64> failure_frame_ms { 0 };

    auto drainer = Threading::Thread::construct("Consumer Drainer"sv, [&]() -> intptr_t {
        u32 random_state = 0x9e3779b9;
        auto next_random = [&](u32 bound) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            return random_state % bound;
        };

        Optional<AK::Duration> seek_target;
        Optional<AK::Duration> last_timestamp;
        size_t frames_since_seek = 0;
        auto fail = [&](AK::Duration target, AK::Duration frame_timestamp) {
            failure_target_ms.store(target.to_milliseconds(), AK::MemoryOrder::memory_order_relaxed);
            failure_frame_ms.store(frame_timestamp.to_milliseconds(), AK::MemoryOrder::memory_order_relaxed);
            failure_detected.store(true, AK::MemoryOrder::memory_order_relaxed);
        };
        auto seek_to = [&](AK::Duration target) {
            harness->consumer->seek(target);
            seek_target = target;
            last_timestamp = {};
            frames_since_seek = 0;
            seek_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        };

        while (!stop.load(AK::MemoryOrder::memory_order_relaxed)) {
            harness->deliver_messages();

            auto output = harness->consumer->peek();
            if (output.status == PipelineStatus::HaveData) {
                auto timestamp = output.frame->timestamp();
                if (seek_target.has_value() && !last_timestamp.has_value() && timestamp > seek_target.value())
                    fail(seek_target.value(), timestamp);
                if (last_timestamp.has_value() && timestamp <= last_timestamp.value())
                    fail(seek_target.value_or(AK::Duration::zero()), timestamp);
                last_timestamp = timestamp;
                harness->consumer->consume();

                if (++frames_since_seek >= 4 + next_random(8)) {
                    // Mostly short hops forward, which land in the frames still on their way, and some jumps anywhere.
                    if (next_random(4) != 0)
                        seek_to(timestamp + AK::Duration::from_milliseconds(10 + next_random(120)));
                    else
                        seek_to(AK::Duration::from_milliseconds(next_random(4500)));
                }
                continue;
            }
            if (output.status == PipelineStatus::EndOfStream) {
                seek_to(AK::Duration::from_milliseconds(next_random(2000)));
                continue;
            }

            MutexLocker locker { harness->mutex };
            if (harness->pending_messages.is_empty()) {
                while (!harness->signaled && !stop.load(AK::MemoryOrder::memory_order_relaxed) && harness->pending_messages.is_empty())
                    harness->signal.wait();
            }
            harness->signaled = false;
        }
        return 0;
    });
    drainer->start();

    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(1500);
    while (MonotonicTime::now_coarse() < deadline)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    stop.store(true, AK::MemoryOrder::memory_order_relaxed);
    harness->wake();
    (void)drainer->join();

    if (failure_detected.load(AK::MemoryOrder::memory_order_relaxed)) {
        FAIL(MUST(String::formatted("Frame at {}ms delivered out of place after seeking to {}ms",
            failure_frame_ms.load(AK::MemoryOrder::memory_order_relaxed),
            failure_target_ms.load(AK::MemoryOrder::memory_order_relaxed))));
    }
    EXPECT(seek_count.load(AK::MemoryOrder::memory_order_relaxed) > 10);
}

TEST_CASE(video_presentation_server_releases_playback_edge_when_connection_dies)
{
    auto& loop = never_destroyed_event_loop();

    auto manager = PlaybackManager::create(Media::AudioOutputFactory::create_null());
    bool metadata_parsed = false;
    manager->add_media_source(create_demuxer(load_test_file(TEST_CLIP)))->when_resolved([&](auto&) {
        metadata_parsed = true;
    });
    if (!spin_until(loop, [&] { return metadata_parsed; })) {
        FAIL("Timed out waiting for metadata");
        return;
    }
    if (manager->video_tracks().is_empty()) {
        FAIL("Expected a video track");
        return;
    }

    auto track = manager->video_tracks()[0];
    auto handle = manager->reserve_video_sink_handle(track);

    auto paired = MUST(IPC::Transport::create_paired());
    auto server = VideoPresentationServerConnection::construct(move(paired.local));
    auto client = adopt_ref(*new VideoPresentationClientConnection(MUST(paired.remote_handle.create_transport())));
    bool edge_ready = false;
    MUST(client->create_edge(handle, [&](NonnullRefPtr<DisplayingVideoSink>) {
        edge_ready = true;
    }));
    if (!spin_until(loop, [&] { return edge_ready; })) {
        FAIL("Timed out waiting for video edge");
        return;
    }
    EXPECT(manager->track_is_enabled(track));

    server->shutdown();
    EXPECT(!manager->track_is_enabled(track));
}

// Each edge keeps the frames it is lent, so a client that asks for edges without end is refused past the limit, until
// it releases one.
TEST_CASE(video_presentation_client_limits_its_edges)
{
    auto paired = MUST(IPC::Transport::create_paired());
    auto server = VideoPresentationServerConnection::construct(move(paired.local));
    auto client = adopt_ref(*new VideoPresentationClientConnection(MUST(paired.remote_handle.create_transport())));

    Vector<VideoSinkHandle> handles;
    for (size_t i = 0; i < VideoPresentationClientConnection::MAX_EDGE_COUNT; i++) {
        handles.append(allocate_video_sink_handle());
        EXPECT(!client->create_edge(handles.last(), [](NonnullRefPtr<DisplayingVideoSink>) { }).is_error());
    }
    EXPECT(client->create_edge(allocate_video_sink_handle(), [](NonnullRefPtr<DisplayingVideoSink>) { }).is_error());

    client->release_edge(handles.first());
    EXPECT(!client->create_edge(allocate_video_sink_handle(), [](NonnullRefPtr<DisplayingVideoSink>) { }).is_error());
}

// A forward seek landing inside the received lookahead is resolved locally; a seek past it re-fetches.
TEST_CASE(consumer_resolves_forward_seeks_within_the_lookahead)
{
    auto pool = MUST(VideoFramePool::create());
    size_t upstream_seeks = 0;
    auto consumer = RemoteVideoProducer::create(make_consumer_delegates(upstream_seeks));
    give_lookahead(*consumer, *pool);

    consumer->seek(AK::Duration::from_milliseconds(1200));
    EXPECT_EQ(upstream_seeks, 0u);

    consumer->seek(AK::Duration::from_milliseconds(3000));
    EXPECT_EQ(upstream_seeks, 1u);
}

// A backward seek leaves the lookahead behind, so it must re-fetch upstream.
TEST_CASE(consumer_sends_backward_seeks_upstream)
{
    auto pool = MUST(VideoFramePool::create());
    size_t upstream_seeks = 0;
    auto consumer = RemoteVideoProducer::create(make_consumer_delegates(upstream_seeks));
    give_lookahead(*consumer, *pool);

    consumer->seek(AK::Duration::from_milliseconds(500));
    EXPECT_EQ(upstream_seeks, 1u);
}

// The seek must reach upstream before the stale frames are released, naming the newest frame received so far, and a
// frame of the new seek that arrives in between must be kept.
TEST_CASE(consumer_requests_seeks_before_releasing_stale_frames)
{
    auto pool = MUST(VideoFramePool::create());
    Vector<StringView> events;
    Optional<u64> requested_newest_received_lend_id;
    RefPtr<RemoteVideoProducer> consumer;
    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [&](AK::Duration, InvalidateHeldData, u64 newest_received_lend_id) {
        events.append("seek"sv);
        requested_newest_received_lend_id = newest_received_lend_id;
        consumer->receive_frame(5, 1, create_received_frame(*pool, AK::Duration::from_milliseconds(500)));
    };
    consumer = RemoteVideoProducer::create(move(delegates));
    Function<void()> on_release = [&] { events.append("release"sv); };
    give_lookahead(*consumer, *pool, &on_release);

    consumer->seek(AK::Duration::from_milliseconds(500));

    EXPECT_EQ(events, (Vector<StringView> { "seek"sv, "release"sv, "release"sv, "release"sv, "release"sv }));
    EXPECT_EQ(requested_newest_received_lend_id, 4u);
    auto output = consumer->peek();
    EXPECT_EQ(output.status, PipelineStatus::HaveData);
    EXPECT_EQ(output.frame->timestamp(), AK::Duration::from_milliseconds(500));
}

// With the end of stream received, no frames exist beyond the lookahead for the producer to
// re-emit, so a seek past the end must resolve locally instead of discarding the queued tail.
TEST_CASE(consumer_resolves_seeks_past_the_end_of_stream_locally)
{
    auto pool = MUST(VideoFramePool::create());
    size_t upstream_seeks = 0;
    auto consumer = RemoteVideoProducer::create(make_consumer_delegates(upstream_seeks));
    give_lookahead(*consumer, *pool);
    consumer->receive_status(PipelineStatus::EndOfStream, 0);

    consumer->seek(AK::Duration::from_milliseconds(3000));
    EXPECT_EQ(upstream_seeks, 0u);

    // Backward seeks below the queued tail still re-fetch, end of stream or not.
    consumer->seek(AK::Duration::from_milliseconds(500));
    EXPECT_EQ(upstream_seeks, 1u);
}

// Observing a suspended edge must release every queued frame, so that the suspended producer's storage is actually
// reclaimed; resuming demand-seeks past those frames anyway.
TEST_CASE(consumer_releases_queued_frames_on_suspension)
{
    auto pool = MUST(VideoFramePool::create());
    size_t upstream_seeks = 0;
    size_t released_frames = 0;
    Function<void()> on_release = [&] { released_frames++; };
    auto consumer = RemoteVideoProducer::create(make_consumer_delegates(upstream_seeks));
    give_lookahead(*consumer, *pool, &on_release);

    consumer->receive_status(PipelineStatus::Suspended, 0);

    EXPECT_EQ(released_frames, 4u);
    auto output = consumer->peek();
    EXPECT(output.frame == nullptr);
    EXPECT_EQ(output.status, PipelineStatus::Suspended);
}

// Frames lent under a seek the consumer has moved past are released as they arrive, and so are frames past the
// queue's limit, so that a peer cannot make the consumer hold its storage without end.
TEST_CASE(consumer_releases_frames_it_does_not_queue)
{
    auto pool = MUST(VideoFramePool::create());
    size_t upstream_seeks = 0;
    size_t released_frames = 0;
    auto consumer = RemoteVideoProducer::create(make_consumer_delegates(upstream_seeks));

    consumer->seek(AK::Duration::from_seconds(5));
    consumer->receive_frame(1, 0, create_received_frame(*pool, AK::Duration::zero(), [&] { released_frames++; }));
    EXPECT_EQ(released_frames, 1u);
    EXPECT_EQ(consumer->peek().status, PipelineStatus::Pending);

    for (u64 i = 0; i < RemoteVideoProducer::MAX_QUEUED_FRAME_COUNT + 2; i++)
        consumer->receive_frame(i + 2, 1, create_received_frame(*pool, AK::Duration::from_milliseconds(5000 + i), [&] { released_frames++; }));
    EXPECT_EQ(released_frames, 3u);
}

// The sink's input has moved past the frames on their way to the consumer when the consumer asks for a seek. If those
// frames hold the target, they are lent again under the seek's stamp instead of seeking the input back.
TEST_CASE(sink_lends_frames_on_their_way_again_for_a_seek_into_them)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    for (int i = 0; i < 4; i++)
        producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(i * 33)));
    MUST(sink->connect_input(producer));
    sink->start_input();
    EXPECT(spin_until(loop, [&] { return recorder->count() == 4; }));

    // The consumer had received only the first frame.
    auto newest_received_lend_id = recorder->at(0).lend_id;
    sink->seek_upstream(AK::Duration::from_milliseconds(70), InvalidateHeldData::No, newest_received_lend_id);
    EXPECT_EQ(producer->seek_count(), 0u);

    if (!spin_until(loop, [&] { return recorder->count() == 6; })) {
        FAIL("The frames on their way were not lent again");
        return;
    }
    EXPECT_EQ(recorder->at(4).seek_id, 1u);
    EXPECT_EQ(recorder->at(4).metadata.timestamp, AK::Duration::from_milliseconds(66));
    EXPECT_EQ(recorder->at(5).metadata.timestamp, AK::Duration::from_milliseconds(99));
    EXPECT(recorder->at(5).lend_id > recorder->at(3).lend_id);
}

// A seek outside the frames on their way to the consumer goes to the input, whether those frames end before the target
// or the consumer had received them all.
TEST_CASE(sink_seeks_its_input_for_seeks_outside_the_frames_on_their_way)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    for (int i = 0; i < 4; i++)
        producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(i * 33)));
    MUST(sink->connect_input(producer));
    sink->start_input();
    EXPECT(spin_until(loop, [&] { return recorder->count() == 4; }));

    sink->seek_upstream(AK::Duration::from_seconds(5), InvalidateHeldData::No, recorder->at(0).lend_id);
    EXPECT_EQ(producer->seek_count(), 1u);

    sink->seek_upstream(AK::Duration::from_milliseconds(70), InvalidateHeldData::No, recorder->at(3).lend_id);
    EXPECT_EQ(producer->seek_count(), 2u);

    sink->seek_upstream(AK::Duration::from_milliseconds(70), InvalidateHeldData::Yes, recorder->at(0).lend_id);
    EXPECT_EQ(producer->seek_count(), 3u);
}

// A seek the consumer holds nothing for goes to the input right away, and the consumer's request for it, which
// follows, is already answered; a request for anywhere else is a new seek.
TEST_CASE(sink_seeks_its_input_directly_when_the_consumer_holds_nothing_for_the_seek)
{
    auto sink = make_sink_with_inert_delegates();
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    sink->seek(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);
    sink->seek_upstream(AK::Duration::from_seconds(1), InvalidateHeldData::No, 0);
    EXPECT_EQ(producer->seek_count(), 1u);

    sink->seek_upstream(AK::Duration::from_seconds(2), InvalidateHeldData::No, 0);
    EXPECT_EQ(producer->seek_count(), 2u);
}

// The consumer may seek on its own while a direct seek awaits its request, as when it starts ticking again. Its
// request supersedes the direct seek, so the input follows the consumer, pumping resumes under the request's stamp,
// and the request for the direct seek, which arrives after, is a seek of its own again.
TEST_CASE(a_request_for_another_seek_supersedes_the_direct_seek)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->start_input();

    sink->seek(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);

    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_seconds(2)));
    sink->seek_upstream(AK::Duration::from_seconds(2), InvalidateHeldData::No, 0);
    EXPECT_EQ(producer->seek_count(), 2u);
    EXPECT(spin_until(loop, [&] { return recorder->count() == 1; }));
    EXPECT_EQ(recorder->at(0).seek_id, 1u);

    sink->seek_upstream(AK::Duration::from_seconds(1), InvalidateHeldData::No, recorder->at(0).lend_id);
    EXPECT_EQ(producer->seek_count(), 3u);
}

// The frames lent across the edge may satisfy a seek on the consumer's side, so such a seek waits for the consumer's
// decision.
TEST_CASE(sink_leaves_seeks_within_the_lent_frames_to_the_consumer)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));
    MUST(sink->connect_input(producer));
    sink->start_input();
    EXPECT(spin_until(loop, [&] { return recorder->count() == 2; }));

    sink->seek(AK::Duration::from_milliseconds(20));
    EXPECT_EQ(producer->seek_count(), 0u);
    sink->seek_upstream(AK::Duration::from_milliseconds(20), InvalidateHeldData::No, recorder->at(1).lend_id);
    EXPECT_EQ(producer->seek_count(), 1u);

    sink->seek(AK::Duration::from_milliseconds(500));
    EXPECT_EQ(producer->seek_count(), 2u);
}

// Between the direct seek and the consumer's request for it, a frame of the new position would be stamped with the
// seek before it, so pumping waits for the request.
TEST_CASE(pump_holds_off_after_a_direct_seek_until_the_consumer_requests_it)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->seek(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);
    sink->start_input();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));
    producer->wake();

    auto hold_off_deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(100);
    while (MonotonicTime::now_coarse() < hold_off_deadline) {
        EXPECT_EQ(recorder->count(), 0u);
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    sink->seek_upstream(AK::Duration::from_seconds(1), InvalidateHeldData::No, 0);
    EXPECT_EQ(producer->seek_count(), 1u);
    EXPECT(spin_until(loop, [&] { return recorder->count() > 0; }));
    EXPECT_EQ(recorder->at(0).seek_id, 1u);
}

// The pump stops taking frames from its input once the limit of frames lent at once is reached, until one comes back.
TEST_CASE(pump_waits_for_a_release_once_it_has_lent_its_limit)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create(VideoFramePool::DEFAULT_BYTE_BUDGET));
    auto producer = ScriptedVideoProducer::create();
    auto frame = create_pooled_frame(*pool, AK::Duration::zero());
    for (size_t i = 0; i < RemoteVideoSink::MAX_LENT_FRAME_COUNT + 1; i++)
        producer->append_frame(frame);
    MUST(sink->connect_input(producer));
    sink->start_input();

    EXPECT(spin_until(loop, [&] { return recorder->count() == RemoteVideoSink::MAX_LENT_FRAME_COUNT; }));
    auto hold_off_deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(100);
    while (MonotonicTime::now_coarse() < hold_off_deadline) {
        EXPECT_EQ(recorder->count(), RemoteVideoSink::MAX_LENT_FRAME_COUNT);
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    sink->release_frame(recorder->at(0).lend_id);
    EXPECT(spin_until(loop, [&] { return recorder->count() == RemoteVideoSink::MAX_LENT_FRAME_COUNT + 1; }));
}

// A stopped pump lends nothing more, but the frames it lent stay lent until the consumer releases them.
TEST_CASE(stopped_pump_keeps_its_lent_frames_until_they_are_released)
{
    auto& loop = never_destroyed_event_loop();
    auto recorder = make_ref_counted<LentFrameRecorder>();
    auto sink = make_recorded_sink(*recorder);

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));
    MUST(sink->connect_input(producer));
    sink->start_input();
    EXPECT(spin_until(loop, [&] { return recorder->count() == 2; }));

    sink->stop();
    EXPECT(sink->has_lent_frames());

    sink->release_frame(recorder->at(0).lend_id);
    EXPECT(sink->has_lent_frames());
    sink->release_frame(recorder->at(1).lend_id);
    EXPECT(!sink->has_lent_frames());
}
