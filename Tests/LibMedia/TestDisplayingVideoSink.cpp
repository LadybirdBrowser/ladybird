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
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>
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
    return make_ref_counted<Media::VideoFrame>(timestamp, duration, Gfx::Size<u32>(frame_size), bit_depth, subsampling, Media::CodingIndependentCodePoints {}, move(acquired.storage));
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

TEST_CASE(seek_past_the_cached_frames_waits_for_the_frame_at_the_target)
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

    // The target lies after the cached frames end, but before the later one's conservative end.
    clock->seek(AK::Duration::from_milliseconds(1080));
    dispatched_statuses.clear();
    sink->seek(AK::Duration::from_milliseconds(1080));
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(dispatched_statuses.is_empty());

    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(1066), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1066));
    EXPECT_EQ(dispatched_statuses, (Vector { Media::PipelineStatus::HaveData }));
}

TEST_CASE(frames_passed_while_waiting_on_a_late_frame_are_counted_as_dropped)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // Both frames are late, and the second waits for a frame after it to replace it.
    clock->seek(AK::Duration::from_milliseconds(1200));
    for (auto timestamp : { 1040, 1080 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(40)));
    EXPECT(!sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1000));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 0u);
    EXPECT_EQ(frame_counts.dropped, 1u);
    EXPECT(sink->take_frame_counts().is_zero());
}

TEST_CASE(frames_passed_by_an_update_landing_on_time_are_counted_as_presented)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(4)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // Updates spaced further apart than the frames make the oldest ones late, but the frame landed on is on time.
    clock->seek(AK::Duration::from_milliseconds(1016));
    for (auto timestamp : { 1004, 1008, 1012, 1016 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(4)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1016));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 4u);
    EXPECT_EQ(frame_counts.dropped, 0u);
}

TEST_CASE(frames_passed_by_a_seek_within_the_cached_frames_are_not_counted)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // A long frame followed by a short one, so that a seek within the long one's conservative end lands after the
    // short one's.
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(100)));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1100), AK::Duration::from_milliseconds(10)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1110), AK::Duration::from_milliseconds(40)));
    clock->seek(AK::Duration::from_milliseconds(1120));
    sink->seek(AK::Duration::from_milliseconds(1120));
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1110));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 1u);
    EXPECT_EQ(frame_counts.dropped, 0u);
}

TEST_CASE(resuming_ticking_seeks_past_frames_held_behind_the_clock)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    auto seeks_before_resuming = producer->seek_count();

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1040), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // The displayed frame still covers the clock, so there is nothing to skip.
    sink->set_ticking(false);
    sink->set_ticking(true);
    EXPECT_EQ(producer->seek_count(), seeks_before_resuming);

    sink->set_ticking(false);
    clock->seek(AK::Duration::from_milliseconds(1200));
    sink->set_ticking(true);
    EXPECT_EQ(producer->seek_count(), seeks_before_resuming + 1);

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1200), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1200));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 1u);
    EXPECT_EQ(frame_counts.dropped, 0u);
}

TEST_CASE(wakes_while_not_ticking_leave_frames_held_behind_the_clock)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    sink->set_ticking(false);
    clock->seek(AK::Duration::from_milliseconds(1200));
    for (auto timestamp : { 1040, 1080, 1120, 1160 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(40)));
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1000));
    EXPECT(sink->take_frame_counts().is_zero());

    auto seeks_before_resuming = producer->seek_count();
    sink->set_ticking(true);
    EXPECT_EQ(producer->seek_count(), seeks_before_resuming + 1);
}

TEST_CASE(seek_while_not_ticking_resolves_once_a_frame_covers_the_target)
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

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    sink->set_ticking(false);
    clock->seek(AK::Duration::from_milliseconds(1200));
    dispatched_statuses.clear();
    sink->seek(AK::Duration::from_milliseconds(1200));

    // The first frame back falls short of the target, so the seek stays pending.
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1120), AK::Duration::from_milliseconds(40)));
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(dispatched_statuses.is_empty());

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1160), AK::Duration::from_milliseconds(80)));
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1160));
    EXPECT_EQ(dispatched_statuses, (Vector { Media::PipelineStatus::HaveData }));
}

TEST_CASE(frames_passed_catching_up_after_resuming_from_suspension_are_not_counted)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();

    // A running clock makes the update resume the input with a seek.
    clock->seek(AK::Duration::from_milliseconds(2000));
    clock->resume();
    auto seeks_before_resuming = producer->seek_count();
    sink->update(MonotonicTime::now());
    EXPECT_EQ(producer->seek_count(), seeks_before_resuming + 1);
    producer->consume();

    // The clock passes the frame at the target before it arrives, and the following frames trail behind it.
    clock->seek(AK::Duration::from_milliseconds(2100));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(2000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    clock->seek(AK::Duration::from_milliseconds(2200));
    for (auto timestamp : { 2040, 2080 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(40)));
    sink->update(MonotonicTime::now());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(2200), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(2200));
    EXPECT_EQ(sink->take_frame_counts().dropped, 0u);
}

TEST_CASE(frames_replaced_within_an_update_are_presented_when_the_last_one_is_on_time)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // Both frames are due and neither is late, so the update passes over the first.
    clock->seek(AK::Duration::from_milliseconds(1090));
    for (auto timestamp : { 1040, 1080 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1080));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 2u);
    EXPECT_EQ(frame_counts.dropped, 0u);
}

TEST_CASE(frames_passed_for_a_late_frame_ending_the_stream_are_counted_as_presented)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // A long frame that is still on time is replaced by a short one that is already late, but is presented because
    // the stream ends after it.
    clock->seek(AK::Duration::from_milliseconds(1150));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1040), AK::Duration::from_milliseconds(100)));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1100), AK::Duration::from_milliseconds(10)));
    producer->append_output(nullptr, Media::PipelineStatus::EndOfStream);
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1100));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 2u);
    EXPECT_EQ(frame_counts.dropped, 0u);
}

TEST_CASE(frames_out_of_order_with_a_passed_frame_are_not_counted)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // The frame at 1020 follows the current frame, but precedes the late frame skipped before it.
    clock->seek(AK::Duration::from_milliseconds(1200));
    for (auto timestamp : { 1040, 1020, 1180 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1180));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 2u);
    EXPECT_EQ(frame_counts.dropped, 0u);
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
