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
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoEdgeQueue.h>
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

// Bridges the two ends of the edge the way the IPC connection eventually will. The sink's pump-thread
// delegates hand slot announcements and wake-ups to the consumer's draining thread; the consumer's
// delegates drive the sink. RemoteVideoProducer is single-threaded, so the pump's delegates only queue
// announcements and signal the draining thread, which is where the consumer is touched.
struct VideoEdgeTestHarness
    : public AtomicRefCounted<VideoEdgeTestHarness>
    , public ThreadSafeWeakable<VideoEdgeTestHarness> {
    struct SlotAnnouncement {
        VideoFramePoolID pool_id;
        u32 slot_index { 0 };
        Core::AnonymousBuffer slot_buffer;
        RefPtr<VideoSurface> surface;
    };

    RefPtr<RemoteVideoSink> sink;
    NonnullRefPtr<VideoFrameSlotDirectory> slot_directory { VideoFrameSlotDirectory::create() };
    RefPtr<RemoteVideoProducer> consumer;

    Mutex mutex;
    ConditionVariable signal { mutex };
    Vector<SlotAnnouncement> pending_announces;
    bool signaled { false };

    void wake()
    {
        MutexLocker locker { mutex };
        signaled = true;
        signal.broadcast();
    }

    void drain_announces()
    {
        Vector<SlotAnnouncement> announces;
        {
            MutexLocker locker { mutex };
            announces = move(pending_announces);
        }
        for (auto& announcement : announces)
            slot_directory->notify_slot_announced(announcement.pool_id, announcement.slot_index, move(announcement.slot_buffer), move(announcement.surface));
        // The pump's delegates only wake this thread; the consumer is single-threaded, so it is notified here.
        consumer->notify_data_available();
    }
};

void wire_full_node(VideoEdgeTestHarness& harness, NonnullRefPtr<VideoProducer> const& decoder)
{
    // The detached pump thread can invoke these delegates while the test scope is being torn down,
    // so they promote a weak reference to keep the harness alive for the duration of the call.
    RemoteVideoSink::Delegates sink_delegates;
    sink_delegates.ring_data_available = [weak = harness.make_weak_ref()] {
        if (auto harness = weak.strong_ref())
            harness->wake();
    };
    sink_delegates.transmit_seek = [](InvalidateHeldData) { };
    sink_delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    sink_delegates.announce_slot = [weak = harness.make_weak_ref()](VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer, RefPtr<VideoSurface> surface) {
        auto harness = weak.strong_ref();
        if (!harness)
            return;
        {
            MutexLocker locker { harness->mutex };
            harness->pending_announces.append({ pool_id, slot_index, move(slot_buffer), move(surface) });
        }
        harness->wake();
    };
    sink_delegates.retire_pool = [](VideoFramePoolID) { };
    harness.sink = MUST(RemoteVideoSink::create(move(sink_delegates)));

    auto ring_fd = MUST(Core::System::dup(harness.sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, harness.sink->edge().header_buffer()));
    RemoteVideoProducer::Delegates consumer_delegates;
    consumer_delegates.request_start = [&harness] { harness.sink->start_input(); };
    consumer_delegates.request_seek = [&harness](AK::Duration timestamp, InvalidateHeldData invalidate_held_data) { harness.sink->seek_upstream(timestamp, invalidate_held_data); };
    consumer_delegates.release_slot = [&harness](VideoFramePoolID pool_id, u32 slot_index) { harness.sink->release_slot(pool_id, slot_index); };
    consumer_delegates.notify_space_available = [&harness] { harness.sink->notify_space_available(); };
    harness.consumer = RemoteVideoProducer::create(move(consumer_edge), harness.slot_directory, move(consumer_delegates));
    harness.consumer->set_wake_handler([&harness] { harness.wake(); });

    // harness.consumer is set before connect_input, whose lock publishes it to the pump before the
    // pump can ring the consumer.
    MUST(harness.sink->connect_input(decoder));
    harness.consumer->start();
}

// Builds a consumer over a controlled edge pre-filled with four frames at 1.0s..1.3s under seek_id 0,
// to exercise the seek fast path deterministically.
NonnullRefPtr<RemoteVideoProducer> make_lookahead_consumer(VideoEdgeQueue& producer_edge, size_t& upstream_seeks)
{
    auto ring_fd = MUST(Core::System::dup(producer_edge.ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, producer_edge.header_buffer()));
    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [&upstream_seeks](AK::Duration, InvalidateHeldData) { upstream_seeks++; };
    delegates.release_slot = [](VideoFramePoolID, u32) { };
    delegates.notify_space_available = [] { };
    auto consumer = RemoteVideoProducer::create(move(consumer_edge), VideoFrameSlotDirectory::create(), move(delegates));

    for (int i = 0; i < 4; i++) {
        VideoFrameHandle handle;
        handle.timestamp = AK::Duration::from_milliseconds(1000 + (i * 100));
        handle.duration = AK::Duration::from_milliseconds(100);
        MUST(producer_edge.enqueue(handle, 0));
    }
    return consumer;
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
        harness->drain_announces();
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
            harness->drain_announces();

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
                    // Seek with this frame still cached (peeked, not consumed) so the drop exercises both
                    // the cached head and the unconsumed edge items.
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
            while (!harness->signaled && !stop.load(AK::MemoryOrder::memory_order_relaxed) && harness->pending_announces.is_empty())
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

TEST_CASE(video_presentation_server_releases_playback_edge_when_connection_dies)
{
    auto& loop = never_destroyed_event_loop();

    auto manager = PlaybackManager::create();
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
    client->create_edge(handle, [&](NonnullRefPtr<DisplayingVideoSink>) {
        edge_ready = true;
    });
    if (!spin_until(loop, [&] { return edge_ready; })) {
        FAIL("Timed out waiting for video edge");
        return;
    }
    EXPECT(manager->track_is_enabled(track));

    server->shutdown();
    EXPECT(!manager->track_is_enabled(track));
}

// A forward seek landing inside the buffered lookahead is resolved locally; a seek past it re-fetches.
TEST_CASE(consumer_resolves_forward_seeks_within_the_lookahead)
{
    auto producer_edge = MUST(VideoEdgeQueue::create());
    size_t upstream_seeks = 0;
    auto consumer = make_lookahead_consumer(producer_edge, upstream_seeks);

    consumer->seek(AK::Duration::from_milliseconds(1200));
    EXPECT_EQ(upstream_seeks, 0u);

    consumer->seek(AK::Duration::from_milliseconds(3000));
    EXPECT_EQ(upstream_seeks, 1u);
}

// A backward seek leaves the lookahead behind, so it must re-fetch upstream.
TEST_CASE(consumer_sends_backward_seeks_upstream)
{
    auto producer_edge = MUST(VideoEdgeQueue::create());
    size_t upstream_seeks = 0;
    auto consumer = make_lookahead_consumer(producer_edge, upstream_seeks);

    consumer->seek(AK::Duration::from_milliseconds(500));
    EXPECT_EQ(upstream_seeks, 1u);
}

// The seek must reach upstream before the stale frames' slots are released, and a frame of the new seek that arrives
// in between must stay in the ring.
TEST_CASE(consumer_requests_seeks_before_releasing_stale_frames)
{
    auto producer_edge = MUST(VideoEdgeQueue::create());
    auto ring_fd = MUST(Core::System::dup(producer_edge.ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, producer_edge.header_buffer()));

    Vector<StringView> events;
    Vector<u32> released_slot_indices;
    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [&](AK::Duration, InvalidateHeldData) {
        events.append("seek"sv);
        VideoFrameHandle handle;
        handle.timestamp = AK::Duration::from_milliseconds(500);
        handle.duration = AK::Duration::from_milliseconds(100);
        handle.slot_index = 4;
        MUST(producer_edge.enqueue(handle, 1));
    };
    delegates.release_slot = [&](VideoFramePoolID, u32 slot_index) {
        events.append("release"sv);
        released_slot_indices.append(slot_index);
    };
    delegates.notify_space_available = [] { };
    auto consumer = RemoteVideoProducer::create(move(consumer_edge), VideoFrameSlotDirectory::create(), move(delegates));

    for (u32 i = 0; i < 4; i++) {
        VideoFrameHandle handle;
        handle.timestamp = AK::Duration::from_milliseconds(1000 + (i * 100));
        handle.duration = AK::Duration::from_milliseconds(100);
        handle.slot_index = i;
        MUST(producer_edge.enqueue(handle, 0));
    }

    consumer->seek(AK::Duration::from_milliseconds(500));

    EXPECT_EQ(events.size(), 5u);
    EXPECT_EQ(events.first(), "seek"sv);
    EXPECT_EQ(released_slot_indices, (Vector<u32> { 0, 1, 2, 3 }));
}

// With the end of stream published, no frames exist beyond the lookahead for the producer to
// re-emit, so a seek past the end must resolve locally instead of discarding the queued tail.
TEST_CASE(consumer_resolves_seeks_past_the_end_of_stream_locally)
{
    auto producer_edge = MUST(VideoEdgeQueue::create());
    size_t upstream_seeks = 0;
    auto consumer = make_lookahead_consumer(producer_edge, upstream_seeks);
    producer_edge.set_status(PipelineStatus::EndOfStream, 0);

    consumer->seek(AK::Duration::from_milliseconds(3000));
    EXPECT_EQ(upstream_seeks, 0u);

    // Backward seeks below the queued tail still re-fetch, end of stream or not.
    consumer->seek(AK::Duration::from_milliseconds(500));
    EXPECT_EQ(upstream_seeks, 1u);
}

// Observing a suspended edge must release every undelivered frame's lend, so that the suspended
// producer's pool memory is actually reclaimed; resuming demand-seeks past those frames anyway.
TEST_CASE(consumer_releases_ring_contents_on_suspension)
{
    auto producer_edge = MUST(VideoEdgeQueue::create());
    auto ring_fd = MUST(Core::System::dup(producer_edge.ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, producer_edge.header_buffer()));

    size_t released_slots = 0;
    size_t space_notifications = 0;
    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [](AK::Duration, InvalidateHeldData) { };
    delegates.release_slot = [&released_slots](VideoFramePoolID, u32) { released_slots++; };
    delegates.notify_space_available = [&space_notifications] { space_notifications++; };
    auto consumer = RemoteVideoProducer::create(move(consumer_edge), VideoFrameSlotDirectory::create(), move(delegates));

    for (u32 i = 0; i < 4; i++) {
        VideoFrameHandle handle;
        handle.timestamp = AK::Duration::from_milliseconds(1000 + (i * 100));
        handle.duration = AK::Duration::from_milliseconds(100);
        handle.slot_index = i;
        MUST(producer_edge.enqueue(handle, 0));
    }
    producer_edge.set_status(PipelineStatus::Suspended, 0);

    consumer->notify_data_available();

    EXPECT_EQ(released_slots, 4u);
    EXPECT_EQ(space_notifications, 1u);
    auto output = consumer->peek();
    EXPECT(output.frame == nullptr);
    EXPECT_EQ(output.status, PipelineStatus::Suspended);
}

namespace {

NonnullRefPtr<VideoFrame> create_pooled_frame(VideoFramePool& pool, AK::Duration timestamp)
{
    static constexpr Gfx::IntSize frame_size { 2, 2 };
    static constexpr u8 bit_depth = 8;
    auto subsampling = Subsampling(true, true);

    auto layout = MUST(frame_plane_layout(frame_size, bit_depth, subsampling));
    auto acquired = pool.try_acquire(layout.total_byte_count).release_value();
    auto slot = MUST(pool.try_adopt_acquired_slot(acquired));

    return make_ref_counted<VideoFrame>(timestamp, AK::Duration::from_milliseconds(33), Gfx::Size<u32>(frame_size), bit_depth, subsampling, CodingIndependentCodePoints {}, move(slot));
}

}

// While a seek request is in flight, the pump must leave the producer's frames where the seek can still find
// them, instead of moving them into the ring under a stamp the consumer has already superseded.
TEST_CASE(pump_holds_off_while_a_seek_request_is_in_flight)
{
    auto& loop = never_destroyed_event_loop();

    RemoteVideoSink::Delegates delegates;
    delegates.ring_data_available = [] { };
    delegates.transmit_seek = [](InvalidateHeldData) { };
    delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    delegates.announce_slot = [](VideoFramePoolID, u32, Core::AnonymousBuffer, RefPtr<VideoSurface>) { };
    delegates.retire_pool = [](VideoFramePoolID) { };
    auto sink = MUST(RemoteVideoSink::create(move(delegates)));

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));

    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));

    // The consumer has requested a seek the pump has not applied yet.
    consumer_edge.set_requested_seek_id(1);
    MUST(sink->connect_input(producer));
    sink->start_input();

    auto hold_off_deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(100);
    while (MonotonicTime::now_coarse() < hold_off_deadline) {
        EXPECT(!consumer_edge.peek().has_value());
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    // Applying the seek lets pumping resume, with the frames stamped for the request.
    sink->seek_upstream(AK::Duration::zero());
    EXPECT(spin_until(loop, [&] { return consumer_edge.peek().has_value(); }));
    EXPECT_EQ(consumer_edge.peek()->seek_id, 1u);
}

namespace {

NonnullRefPtr<RemoteVideoSink> make_sink_with_inert_delegates()
{
    RemoteVideoSink::Delegates delegates;
    delegates.ring_data_available = [] { };
    delegates.transmit_seek = [](InvalidateHeldData) { };
    delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    delegates.announce_slot = [](VideoFramePoolID, u32, Core::AnonymousBuffer, RefPtr<VideoSurface>) { };
    delegates.retire_pool = [](VideoFramePoolID) { };
    return MUST(RemoteVideoSink::create(move(delegates)));
}

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
    sink->seek_upstream(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);

    sink->seek_upstream(AK::Duration::from_seconds(2));
    EXPECT_EQ(producer->seek_count(), 2u);
}

// A request the consumer has already made will seek the input to its own target, so a seek that arrives while the
// request is on its way is left to the consumer.
TEST_CASE(sink_leaves_a_seek_to_the_consumer_while_its_request_is_on_its_way)
{
    auto sink = make_sink_with_inert_delegates();
    auto producer = ScriptedVideoProducer::create();
    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));
    MUST(sink->connect_input(producer));

    consumer_edge.set_requested_seek_id(1);
    sink->seek(AK::Duration::from_seconds(2));
    EXPECT_EQ(producer->seek_count(), 0u);

    sink->seek_upstream(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);
    consumer_edge.set_requested_seek_id(2);
    sink->seek_upstream(AK::Duration::from_seconds(2));
    EXPECT_EQ(producer->seek_count(), 2u);
}

// The consumer may seek on its own while a direct seek awaits its request, as when it starts ticking again. Its
// request supersedes the direct seek, so the input follows the consumer, pumping resumes under the request's stamp,
// and the request for the direct seek, which arrives after, is a seek of its own again.
TEST_CASE(a_request_for_another_seek_supersedes_the_direct_seek)
{
    auto& loop = never_destroyed_event_loop();
    auto sink = make_sink_with_inert_delegates();

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));
    MUST(sink->connect_input(producer));
    sink->start_input();

    sink->seek(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);

    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_seconds(2)));
    consumer_edge.set_requested_seek_id(1);
    sink->seek_upstream(AK::Duration::from_seconds(2));
    EXPECT_EQ(producer->seek_count(), 2u);
    EXPECT(spin_until(loop, [&] { return consumer_edge.peek().has_value(); }));
    EXPECT_EQ(consumer_edge.peek()->seek_id, 1u);

    consumer_edge.set_requested_seek_id(2);
    sink->seek_upstream(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 3u);
}

// The frames lent across the edge may satisfy a seek on the consumer's side, so such a seek waits for the consumer's
// decision.
TEST_CASE(sink_leaves_seeks_within_the_lent_frames_to_the_consumer)
{
    auto& loop = never_destroyed_event_loop();
    auto sink = make_sink_with_inert_delegates();

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));

    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));
    MUST(sink->connect_input(producer));
    sink->start_input();
    EXPECT(spin_until(loop, [&] { return consumer_edge.peek_newest().has_value() && consumer_edge.peek_newest()->handle.timestamp == AK::Duration::from_milliseconds(33); }));

    sink->seek(AK::Duration::from_milliseconds(20));
    EXPECT_EQ(producer->seek_count(), 0u);
    consumer_edge.set_requested_seek_id(1);
    sink->seek_upstream(AK::Duration::from_milliseconds(20));
    EXPECT_EQ(producer->seek_count(), 1u);

    sink->seek(AK::Duration::from_milliseconds(500));
    EXPECT_EQ(producer->seek_count(), 2u);
}

// Between the direct seek and the consumer's request for it, a frame of the new position would be stamped with the
// seek before it, so pumping waits for the request.
TEST_CASE(pump_holds_off_after_a_direct_seek_until_the_consumer_requests_it)
{
    auto& loop = never_destroyed_event_loop();
    auto sink = make_sink_with_inert_delegates();

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();

    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));
    MUST(sink->connect_input(producer));
    sink->seek(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);
    sink->start_input();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));
    producer->wake();

    auto hold_off_deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(100);
    while (MonotonicTime::now_coarse() < hold_off_deadline) {
        EXPECT(!consumer_edge.peek().has_value());
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    consumer_edge.set_requested_seek_id(1);
    sink->seek_upstream(AK::Duration::from_seconds(1));
    EXPECT_EQ(producer->seek_count(), 1u);
    EXPECT(spin_until(loop, [&] { return consumer_edge.peek().has_value(); }));
    EXPECT_EQ(consumer_edge.peek()->seek_id, 1u);
}

// A request the consumer withdraws, having resolved the seek locally after all, must let pumping resume under the
// unchanged stamp once the consumer's next consume wakes the pump.
TEST_CASE(pump_resumes_after_a_withdrawn_seek_request)
{
    auto& loop = never_destroyed_event_loop();

    RemoteVideoSink::Delegates delegates;
    delegates.ring_data_available = [] { };
    delegates.transmit_seek = [](InvalidateHeldData) { };
    delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    delegates.announce_slot = [](VideoFramePoolID, u32, Core::AnonymousBuffer, RefPtr<VideoSurface>) { };
    delegates.retire_pool = [](VideoFramePoolID) { };
    auto sink = MUST(RemoteVideoSink::create(move(delegates)));

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));

    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));

    consumer_edge.set_requested_seek_id(1);
    MUST(sink->connect_input(producer));
    sink->start_input();

    auto hold_off_deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(100);
    while (MonotonicTime::now_coarse() < hold_off_deadline) {
        EXPECT(!consumer_edge.peek().has_value());
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    consumer_edge.set_requested_seek_id(0);
    sink->notify_space_available();
    EXPECT(spin_until(loop, [&] { return consumer_edge.peek().has_value(); }));
    EXPECT_EQ(consumer_edge.peek()->seek_id, 0u);
}

TEST_CASE(stopped_pump_reclaims_only_the_frames_left_in_the_ring)
{
    auto& loop = never_destroyed_event_loop();

    auto retired_pool_count = 0;
    RemoteVideoSink::Delegates delegates;
    delegates.ring_data_available = [] { };
    delegates.transmit_seek = [](InvalidateHeldData) { };
    delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    delegates.announce_slot = [](VideoFramePoolID, u32, Core::AnonymousBuffer, RefPtr<VideoSurface>) { };
    delegates.retire_pool = [&](VideoFramePoolID) { retired_pool_count++; };
    auto sink = MUST(RemoteVideoSink::create(move(delegates)));

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));

    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));

    MUST(sink->connect_input(producer));
    sink->start_input();

    EXPECT(spin_until(loop, [&] {
        auto newest = consumer_edge.peek_newest();
        return newest.has_value() && newest->handle.timestamp == AK::Duration::from_milliseconds(33);
    }));
    auto taken_handle = consumer_edge.peek()->handle;
    consumer_edge.consume();

    sink->stop_and_reclaim_unconsumed_frames();
    EXPECT(sink->has_lent_slots());
    EXPECT_EQ(retired_pool_count, 0);

    sink->release_slot(taken_handle.pool_id, taken_handle.slot_index);
    EXPECT(!sink->has_lent_slots());
    EXPECT_EQ(retired_pool_count, 1);
}

TEST_CASE(consumer_forgets_announced_slots_on_suspension)
{
    auto producer_edge = MUST(VideoEdgeQueue::create());
    auto ring_fd = MUST(Core::System::dup(producer_edge.ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, producer_edge.header_buffer()));

    auto pool = MUST(VideoFramePool::create());
    auto frame = create_pooled_frame(*pool, AK::Duration::from_milliseconds(1000));
    auto handle = VideoFrameHandle::for_frame(*frame);

    auto directory = VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), handle.slot_index, pool->slot_buffer(handle.slot_index), nullptr);

    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [](AK::Duration, InvalidateHeldData) { };
    delegates.release_slot = [](VideoFramePoolID, u32) { };
    delegates.notify_space_available = [] { };
    auto consumer = RemoteVideoProducer::create(move(consumer_edge), directory, move(delegates));

    auto resolved_before_suspension = directory->resolve_frame(handle, [] { });
    EXPECT(resolved_before_suspension != nullptr);

    producer_edge.set_status(PipelineStatus::Suspended, 0);
    consumer->notify_data_available();

    // The decoder that announced this slot is gone, so nothing more resolves against it, while the frame that
    // already did keeps reading what it holds.
    EXPECT(directory->resolve_frame(handle, [] { }) == nullptr);
    EXPECT(resolved_before_suspension->revalidate_backing());
}
