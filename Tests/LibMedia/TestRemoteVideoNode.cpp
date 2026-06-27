/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGfx/YUVData.h>
#include <LibIPC/Transport.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Producers/RemoteVideoProducer.h>
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoEdgeQueue.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>
#include <LibMedia/VideoPresentation/PresentedFramePage.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientConnection.h>
#include <LibMedia/VideoPresentation/VideoPresentationServerConnection.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibSync/Weakable.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include "TestMediaCommon.h"

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
    auto matroska_result = Matroska::MatroskaDemuxer::from_stream(stream);
    if (!matroska_result.is_error())
        return matroska_result.release_value();
    return MUST(FFmpeg::FFmpegDemuxer::from_stream(stream));
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
    , public Sync::Weakable<VideoEdgeTestHarness> {
    struct SlotAnnouncement {
        VideoFramePoolID pool_id;
        u32 slot_index { 0 };
        Core::AnonymousBuffer slot_buffer;
    };

    RefPtr<RemoteVideoSink> sink;
    NonnullRefPtr<VideoFrameSlotDirectory> slot_directory { VideoFrameSlotDirectory::create() };
    RefPtr<RemoteVideoProducer> consumer;

    Sync::Mutex mutex;
    Sync::ConditionVariable signal { mutex };
    Vector<SlotAnnouncement> pending_announces;
    bool signaled { false };

    void wake()
    {
        Sync::MutexLocker locker { mutex };
        signaled = true;
        signal.broadcast();
    }

    void drain_announces()
    {
        Vector<SlotAnnouncement> announces;
        {
            Sync::MutexLocker locker { mutex };
            announces = move(pending_announces);
        }
        for (auto& announcement : announces)
            slot_directory->notify_slot_announced(announcement.pool_id, announcement.slot_index, move(announcement.slot_buffer));
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
    sink_delegates.transmit_seek = [] { };
    sink_delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    sink_delegates.announce_slot = [weak = harness.make_weak_ref()](VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer) {
        auto harness = weak.strong_ref();
        if (!harness)
            return;
        {
            Sync::MutexLocker locker { harness->mutex };
            harness->pending_announces.append({ pool_id, slot_index, move(slot_buffer) });
        }
        harness->wake();
    };
    sink_delegates.retire_pool = [](VideoFramePoolID) { };
    harness.sink = MUST(RemoteVideoSink::create(move(sink_delegates)));

    auto ring_fd = MUST(Core::System::dup(harness.sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, harness.sink->edge().header_buffer()));
    RemoteVideoProducer::Delegates consumer_delegates;
    consumer_delegates.request_start = [&harness] { harness.sink->start_input(); };
    consumer_delegates.request_seek = [&harness](AK::Duration timestamp) { harness.sink->seek_upstream(timestamp); };
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
// with the lookahead upper bound published, to exercise the seek fast path deterministically.
NonnullRefPtr<RemoteVideoProducer> make_lookahead_consumer(VideoEdgeQueue& producer_edge, size_t& upstream_seeks)
{
    auto ring_fd = MUST(Core::System::dup(producer_edge.ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, producer_edge.header_buffer()));
    RemoteVideoProducer::Delegates delegates;
    delegates.request_start = [] { };
    delegates.request_seek = [&upstream_seeks](AK::Duration) { upstream_seeks++; };
    delegates.release_slot = [](VideoFramePoolID, u32) { };
    delegates.notify_space_available = [] { };
    auto consumer = RemoteVideoProducer::create(move(consumer_edge), VideoFrameSlotDirectory::create(), move(delegates));

    auto const frame_duration = AK::Duration::from_milliseconds(100);
    AK::Duration available_end;
    for (int i = 0; i < 4; i++) {
        VideoFrameHandle handle;
        handle.timestamp = AK::Duration::from_milliseconds(1000 + (i * 100));
        handle.duration = frame_duration;
        MUST(producer_edge.enqueue(handle, 0));
        available_end = handle.timestamp + frame_duration.scaled_by(3, 2);
    }
    producer_edge.set_available_upper_bound(available_end, 0);
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

            Sync::MutexLocker locker { harness->mutex };
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
    manager->on_metadata_parsed = [&] {
        metadata_parsed = true;
    };
    manager->add_media_source(create_demuxer(load_test_file(TEST_CLIP)));
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
#ifdef AK_OS_WINDOWS
    auto pid = Core::System::getpid();
    server->transport().set_peer_pid(pid);
    client->transport().set_peer_pid(pid);
#endif

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
    delegates.request_seek = [](AK::Duration) { };
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
    auto yuv_data = MUST(Gfx::YUVData::create(frame_size, bit_depth, subsampling, CodingIndependentCodePoints {},
        acquired.bytes.slice(0, layout.y_size),
        acquired.bytes.slice(layout.u_offset, layout.u_size),
        acquired.bytes.slice(layout.v_offset, layout.v_size)));
    auto slot = MUST(pool.try_adopt_acquired_slot(acquired));

    return make_ref_counted<VideoFrame>(timestamp, AK::Duration::from_milliseconds(33), Gfx::Size<u32>(frame_size), bit_depth, yuv_data, move(slot));
}

}

// A presented-frame handle can outlive its lend when a newer frame's release is processed before the
// page rewrite is read. Resolving such a handle must fall back to the superseding one, never crash.
TEST_CASE(current_frame_ignores_handles_whose_lend_was_released)
{
    auto& loop = never_destroyed_event_loop();

    RemoteVideoSink::Delegates delegates;
    delegates.ring_data_available = [] { };
    delegates.transmit_seek = [] { };
    delegates.transmit_time_reader = [](MediaTimeReader const&) { };
    delegates.announce_slot = [](VideoFramePoolID, u32, Core::AnonymousBuffer) { };
    delegates.retire_pool = [](VideoFramePoolID) { };
    auto sink = MUST(RemoteVideoSink::create(move(delegates)));

    auto pool = MUST(VideoFramePool::create());
    auto producer = ScriptedVideoProducer::create();
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::zero()));
    producer->append_frame(create_pooled_frame(*pool, AK::Duration::from_milliseconds(33)));

    auto ring_fd = MUST(Core::System::dup(sink->edge().ring_fd()));
    auto consumer_edge = MUST(VideoEdgeQueue::create(ring_fd, sink->edge().header_buffer()));

    MUST(sink->connect_input(producer));
    sink->start_input();

    Vector<VideoFrameHandle> handles;
    EXPECT(spin_until(loop, [&] {
        while (handles.size() < 2) {
            auto item = consumer_edge.peek();
            if (!item.has_value())
                break;
            handles.append(item->handle);
            consumer_edge.consume();
        }
        return handles.size() == 2;
    }));

    PresentedFramePage page = sink->presented_frame_page();

    // While its lend is live, the presented handle resolves.
    page.store(handles[0]);
    EXPECT(sink->current_frame() != nullptr);

    // Present the second frame and release its lend, as a newer present racing the page read would;
    // the page now holds a stale handle while the pool keeps the first frame's lend.
    page.store(handles[1]);
    sink->release_slot(handles[1].pool_id, handles[1].slot_index);
    EXPECT(sink->current_frame() == nullptr);
}
