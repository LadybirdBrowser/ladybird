/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

namespace {

NonnullRefPtr<Media::VideoFrame> create_test_frame(Media::VideoFramePool& pool, AK::Duration timestamp, AK::Duration duration)
{
    static constexpr Gfx::IntSize frame_size { 2, 2 };
    static constexpr u8 bit_depth = 8;
    auto subsampling = Media::Subsampling(true, true);

    auto plane_sizes = MUST(Gfx::YUVData::plane_sizes(frame_size, bit_depth, subsampling));
    auto acquired = pool.try_acquire(plane_sizes.total).release_value();
    auto slot = MUST(pool.try_adopt_acquired_slot(acquired));

    return make_ref_counted<Media::VideoFrame>(timestamp, duration, Gfx::Size<u32>(frame_size), bit_depth, subsampling, Media::CodingIndependentCodePoints {}, move(slot));
}

}

TEST_CASE(seek_resolving_frame_is_presented_even_when_stale)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // The producer resolves the seek with a frame that covers the target, but whose presentation
    // interval has already passed relative to the clock.
    sink->seek(AK::Duration::from_milliseconds(900));
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(900), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);

    auto result = sink->update(MonotonicTime::now());
    EXPECT(result.new_frame_available);

    auto current_frame = sink->current_frame();
    EXPECT(current_frame != nullptr);
    EXPECT_EQ(current_frame->timestamp(), AK::Duration::from_milliseconds(900));
}

TEST_CASE(suspension_wake_releases_the_prefetched_frame)
{
    never_destroyed_event_loop();

    size_t freed_slots = 0;
    auto pool = MUST(Media::VideoFramePool::create());
    pool->set_slot_freed_callback([&freed_slots] { freed_slots++; });
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // Present the frame covering the current time, leaving the following frame prefetched.
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1033), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    auto result = sink->update(MonotonicTime::now());
    EXPECT(result.new_frame_available);
    EXPECT_EQ(freed_slots, 0u);

    // The suspension wake must release the prefetched frame's slot while keeping the displayed frame.
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    EXPECT_EQ(freed_slots, 1u);

    // Ticking past the prefetched frame's timestamp must not present it after the suspension.
    clock->seek(AK::Duration::from_milliseconds(1040));
    result = sink->update(MonotonicTime::now());
    EXPECT(!result.new_frame_available);
    auto current_frame = sink->current_frame();
    EXPECT(current_frame != nullptr);
    EXPECT_EQ(current_frame->timestamp(), AK::Duration::from_milliseconds(1000));
}

TEST_CASE(updates_are_required_until_the_clock_stops_and_frames_are_presented)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // A seek in flight keeps updates required while the clock is stopped.
    sink->seek(AK::Duration::from_milliseconds(1000));
    EXPECT(sink->update(MonotonicTime::now()).may_require_updates);

    // Resolving the seek with a presentable frame leaves nothing due at the stopped clock.
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    auto result = sink->update(MonotonicTime::now());
    EXPECT(result.new_frame_available);
    EXPECT(!result.may_require_updates);

    // A running clock requires updates regardless of pending frames.
    clock->resume();
    EXPECT(sink->update(MonotonicTime::now()).may_require_updates);

    clock->pause();
    EXPECT(!sink->update(MonotonicTime::now()).may_require_updates);
}

TEST_CASE(paused_sink_before_its_first_frame_stays_suspended)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // The first frame starts after the paused time, so the seek resolves with nothing to present yet.
    sink->seek(AK::Duration::zero());
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(40), AK::Duration::from_milliseconds(40)), Media::PipelineStatus::HaveData);
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(sink->current_frame() == nullptr);
    EXPECT_EQ(dispatched_statuses, (Vector { Media::PipelineStatus::HaveData }));

    // A suspension while paused must not be undone, since no frame is due until the clock moves.
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    auto seek_count_at_suspension = producer->seek_count();
    for (auto i = 0; i < 3; i++)
        EXPECT(!sink->update(MonotonicTime::now()).may_require_updates);
    EXPECT_EQ(producer->seek_count(), seek_count_at_suspension);

    // Once the clock advances, the sink needs its frame again and resumes the input.
    clock->resume();
    sink->update(MonotonicTime::now());
    EXPECT_EQ(producer->seek_count(), seek_count_at_suspension + 1);
}

TEST_CASE(seek_within_the_displayed_frame_resumes_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    auto seek_count_before_seek = producer->seek_count();
    dispatched_statuses.clear();
    sink->seek(AK::Duration::from_milliseconds(1010));
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(producer->seek_count(), seek_count_before_seek + 1);
    EXPECT(dispatched_statuses.is_empty());
}

TEST_CASE(held_late_frame_waiting_on_input_reports_pending)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1033), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(dispatched_statuses, (Vector { Media::PipelineStatus::HaveData }));

    // The clock passes the held frame while the input has nothing after it, so the sink is waiting for data.
    clock->seek(AK::Duration::from_milliseconds(1100));
    dispatched_statuses.clear();
    EXPECT(!sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1000));
    EXPECT_EQ(dispatched_statuses, (Vector { Media::PipelineStatus::Pending }));
}

// The input reaching its end leaves the last frame on screen until its interval ends, and only an update after that
// can report the end of the stream.
TEST_CASE(updates_are_required_until_the_last_frame_finishes_presenting)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    sink->seek(AK::Duration::from_milliseconds(1000));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(100)));
    producer->append_output(nullptr, Media::PipelineStatus::EndOfStream);
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);

    clock->resume();
    EXPECT(sink->update(MonotonicTime::now()).may_require_updates);

    clock->seek(AK::Duration::from_milliseconds(1200));
    EXPECT(!sink->update(MonotonicTime::now()).may_require_updates);
}
