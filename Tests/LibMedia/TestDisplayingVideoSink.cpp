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
    return make_ref_counted<Media::VideoFrame>(timestamp, duration, Gfx::Size<u32>(frame_size), bit_depth, subsampling, Media::ColorMetadata {}, move(acquired.storage));
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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status, Media::PipelineCheckpoint) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

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
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status, Media::PipelineCheckpoint) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

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
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status, Media::PipelineCheckpoint) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

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
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status, Media::PipelineCheckpoint) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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

TEST_CASE(frame_past_its_duration_is_presented_until_the_next_frame_is_due)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    // With reordered frames, a container can give a frame a duration of a single tick, although the frame after it
    // isn't due for another 40ms.
    clock->seek(AK::Duration::from_milliseconds(1060));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1040), AK::Duration::from_milliseconds(1)));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1080), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1040));
    auto frame_counts = sink->take_frame_counts();
    EXPECT_EQ(frame_counts.presented, 1u);
    EXPECT_EQ(frame_counts.dropped, 0u);
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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_ticking(true);
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

// A sink that is not ticking reports its input's suspension at once, while the status of its last update is still waiting
// to be dispatched. That status is older than the suspension, so it must not be dispatched after it.
TEST_CASE(a_suspension_is_not_followed_by_the_status_it_replaced)
{
    auto& loop = never_destroyed_event_loop();

    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status, Media::PipelineCheckpoint) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);
    sink->set_ticking(false);

    producer->append_output(nullptr, Media::PipelineStatus::Blocked);
    (void)sink->update(MonotonicTime::now());

    producer->consume();
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    EXPECT(!dispatched_statuses.is_empty());
    if (!dispatched_statuses.is_empty())
        EXPECT_EQ(dispatched_statuses.last(), Media::PipelineStatus::Suspended);
}

// A sink reports its status in a deferred task, so a wake can arrive before that report is dispatched. The sink is
// already presenting a frame by then, and must not take further frames while it is not ticking.
TEST_CASE(wakes_before_a_status_is_dispatched_leave_frames_held_behind_the_clock)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    EXPECT_EQ(sink->take_frame_counts().presented, 1u);

    sink->set_ticking(false);
    clock->seek(AK::Duration::from_milliseconds(1200));
    for (auto timestamp : { 1040, 1080, 1120, 1160 })
        producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(timestamp), AK::Duration::from_milliseconds(40)));
    producer->wake();
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1000));
    EXPECT(sink->take_frame_counts().is_zero());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
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
    sink->set_ticking(true);

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
    sink->set_state_change_handler([&dispatched_statuses](Media::PipelineStatus status, Media::PipelineCheckpoint) {
        dispatched_statuses.append(status);
    });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

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
    sink->set_ticking(true);

    sink->seek(AK::Duration::from_milliseconds(1000));
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(100)));
    producer->append_output(nullptr, Media::PipelineStatus::EndOfStream);
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);

    clock->resume();
    EXPECT(sink->update(MonotonicTime::now()).may_require_updates);

    clock->seek(AK::Duration::from_milliseconds(1200));
    EXPECT(!sink->update(MonotonicTime::now()).may_require_updates);
}

// Until the sink knows whether it ticks, starting its input either way could be wrong, so it holds the input and any
// seeks back and reports that it is waiting, which keeps the clock from starting.
TEST_CASE(undecided_sink_leaves_its_input_unstarted)
{
    auto& loop = never_destroyed_event_loop();

    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&](Media::PipelineStatus status, Media::PipelineCheckpoint) { dispatched_statuses.append(status); });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    clock->seek(AK::Duration::from_milliseconds(500));
    sink->seek(AK::Duration::from_milliseconds(500));
    (void)sink->update(MonotonicTime::now());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!sink->is_ticking());
    EXPECT_EQ(producer->start_count(), 0u);
    EXPECT_EQ(producer->suspend_count(), 0u);
    EXPECT_EQ(producer->seek_count(), 0u);
    EXPECT(dispatched_statuses.is_empty());

    sink->set_ticking(true);
    EXPECT_EQ(producer->start_count(), 1u);
    EXPECT_EQ(producer->suspend_count(), 0u);
    EXPECT_EQ(producer->seek_count(), 1u);
}

TEST_CASE(sink_first_decided_not_to_tick_suspends_its_input)
{
    auto& loop = never_destroyed_event_loop();

    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&](Media::PipelineStatus status, Media::PipelineCheckpoint) { dispatched_statuses.append(status); });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->seek(AK::Duration::zero());

    sink->set_ticking(false);
    EXPECT_EQ(producer->start_count(), 0u);
    EXPECT_EQ(producer->suspend_count(), 1u);
    EXPECT_EQ(producer->seek_count(), 0u);

    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(dispatched_statuses, Vector { Media::PipelineStatus::Suspended });

    // Playing on while unticked leaves the input suspended.
    clock->resume();
    sink->notify_clock_resumed();
    EXPECT_EQ(producer->seek_count(), 0u);

    // Ticking resumes the input at the current time.
    sink->set_ticking(true);
    EXPECT_EQ(producer->seek_count(), 1u);
}

// A sink that is connected after its ticking was decided starts its input right away.
TEST_CASE(sink_decided_before_connecting_starts_its_input_on_connect)
{
    never_destroyed_event_loop();

    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto ticking_sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    ticking_sink->set_ticking(true);
    auto ticking_producer = ScriptedVideoProducer::create();
    MUST(ticking_sink->connect_input(ticking_producer));
    EXPECT_EQ(ticking_producer->start_count(), 1u);
    EXPECT_EQ(ticking_producer->seek_count(), 1u);

    auto unticked_sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    unticked_sink->set_ticking(false);
    auto unticked_producer = ScriptedVideoProducer::create();
    MUST(unticked_sink->connect_input(unticked_producer));
    EXPECT_EQ(unticked_producer->suspend_count(), 1u);
    EXPECT_EQ(unticked_producer->seek_count(), 0u);
}

// Seeking a sink that isn't ticking would otherwise wake its suspended input only to decode frames that nothing shows.
TEST_CASE(unticked_sink_resolves_a_seek_without_waking_its_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineStatus> dispatched_statuses;
    sink->set_state_change_handler([&](Media::PipelineStatus status, Media::PipelineCheckpoint) { dispatched_statuses.append(status); });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);

    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    EXPECT(sink->update(MonotonicTime::now()).new_frame_available);
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    sink->set_ticking(false);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    auto seeks_before = producer->seek_count();

    dispatched_statuses.clear();
    clock->seek(AK::Duration::zero());
    sink->seek(AK::Duration::zero());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(producer->seek_count(), seeks_before);
    EXPECT_EQ(dispatched_statuses, Vector { Media::PipelineStatus::Suspended });
    // The frame from before the seek stays until the sink catches up.
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1000));

    sink->set_ticking(true);
    EXPECT_EQ(producer->seek_count(), seeks_before + 1);
    EXPECT_EQ(producer->last_seek_timestamp(), AK::Duration::zero());
}

// The frame a sink keeps may show data that a seek invalidates, so such a seek reaches the input even when it isn't
// ticking.
TEST_CASE(unticked_sink_passes_on_a_seek_that_invalidates_held_data)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);
    producer->append_frame(create_test_frame(*pool, AK::Duration::zero(), AK::Duration::from_milliseconds(40)));
    (void)sink->update(MonotonicTime::now());
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    sink->set_ticking(false);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    auto seeks_before = producer->seek_count();
    sink->seek(AK::Duration::zero(), Media::InvalidateHeldData::Yes);
    EXPECT_EQ(producer->seek_count(), seeks_before + 1);
}

TEST_CASE(frame_is_up_to_date_once_a_ticking_sink_has_caught_up)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    EXPECT(!sink->frame_is_up_to_date());

    // A sink that has just begun ticking has yet to bring its frame to the clock, even once it has updated.
    sink->set_ticking(true);
    EXPECT(!sink->frame_is_up_to_date());
    (void)sink->update(MonotonicTime::now());
    EXPECT(!sink->frame_is_up_to_date());
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    (void)sink->update(MonotonicTime::now());
    EXPECT(sink->frame_is_up_to_date());

    // A seek from upstream leaves the frame from before it, which is up to date until the seek resolves.
    sink->seek(AK::Duration::from_milliseconds(3000));
    EXPECT(sink->frame_is_up_to_date());
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(3000), AK::Duration::from_milliseconds(40)));
    clock->seek(AK::Duration::from_milliseconds(3000));
    (void)sink->update(MonotonicTime::now());
    EXPECT(sink->frame_is_up_to_date());

    sink->set_ticking(false);
    EXPECT(!sink->frame_is_up_to_date());

    // Catching up to a clock that moved while the sink wasn't ticking takes a seek of the sink's own.
    clock->seek(AK::Duration::from_milliseconds(5000));
    sink->set_ticking(true);
    (void)sink->update(MonotonicTime::now());
    EXPECT(!sink->frame_is_up_to_date());
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(5000), AK::Duration::from_milliseconds(40)));
    (void)sink->update(MonotonicTime::now());
    EXPECT(sink->frame_is_up_to_date());
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(5000));

    // A clock that stays within the frame needs no catching up.
    sink->set_ticking(false);
    sink->set_ticking(true);
    (void)sink->update(MonotonicTime::now());
    EXPECT(sink->frame_is_up_to_date());
}

// A screenshot catches a sink up to its clock without ticking it.
TEST_CASE(unticked_sink_catches_up_to_its_clock_on_request)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(false);
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    // Paused at a time the sink has no frame for, nothing but a request makes it seek.
    clock->seek(AK::Duration::from_milliseconds(2000));
    (void)sink->update(MonotonicTime::now());
    EXPECT_EQ(producer->seek_count(), 0u);

    sink->catch_up_to_clock();
    EXPECT_EQ(producer->seek_count(), 1u);
    EXPECT(sink->is_catching_up());

    producer->consume();
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(2000), AK::Duration::from_milliseconds(40)));
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!sink->is_catching_up());
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(2000));
    EXPECT(!sink->frame_is_up_to_date());

    // A frame that already shows the clock's time needs no seek.
    sink->catch_up_to_clock();
    EXPECT_EQ(producer->seek_count(), 1u);
    EXPECT(!sink->is_catching_up());
}

TEST_CASE(sink_reports_changes_to_its_freshness)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    size_t changes = 0;
    sink->set_on_freshness_changed([&] { changes++; });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // An undecided sink and an unticked one are equally behind the clock.
    sink->set_ticking(false);
    EXPECT_EQ(changes, 0u);

    // Ticking starts a catch-up, and the frame is up to date once that is done.
    clock->seek(AK::Duration::from_milliseconds(1000));
    sink->set_ticking(true);
    EXPECT_EQ(changes, 1u);
    EXPECT(sink->is_catching_up());
    producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(sink->frame_is_up_to_date());
    EXPECT_EQ(changes, 2u);

    sink->set_ticking(true);
    EXPECT_EQ(changes, 2u);
    sink->set_ticking(false);
    EXPECT_EQ(changes, 3u);
}

// A blocked catch-up isn't counted as catching up, so an input that suspends during it changes the sink's freshness.
TEST_CASE(sink_reports_its_freshness_when_its_input_suspends_during_a_blocked_catch_up)
{
    auto& loop = never_destroyed_event_loop();

    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    size_t changes = 0;
    sink->set_on_freshness_changed([&] { changes++; });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(false);
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    clock->seek(AK::Duration::from_milliseconds(2000));
    sink->catch_up_to_clock();
    producer->consume();
    producer->append_output(nullptr, Media::PipelineStatus::Blocked);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!sink->is_catching_up());

    auto changes_before_suspending = changes;
    producer->consume();
    producer->append_output(nullptr, Media::PipelineStatus::Suspended);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(sink->is_catching_up());
    EXPECT_EQ(changes, changes_before_suspending + 1);
}

TEST_CASE(sink_reconnected_to_a_new_input_takes_its_frames)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto first_producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(first_producer));
    sink->set_ticking(true);

    // The frame after the current one waits in the sink until the clock reaches it.
    first_producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    first_producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1040), AK::Duration::from_milliseconds(40)));
    (void)sink->update(MonotonicTime::now());
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::from_milliseconds(1000));
    EXPECT(sink->frame_is_up_to_date());
    sink->disconnect_input(first_producer);

    // The new input starts with a catch-up, which the sink reports.
    size_t changes = 0;
    sink->set_on_freshness_changed([&] { changes++; });
    auto second_producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(second_producer));
    EXPECT(!sink->frame_is_up_to_date());
    EXPECT_EQ(changes, 1u);
    auto frame = create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40));
    second_producer->append_frame(frame);
    (void)sink->update(MonotonicTime::now());
    EXPECT(!sink->is_catching_up());
    EXPECT_EQ(sink->current_frame().ptr(), frame.ptr());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}

// A catch-up that the previous input left blocked says nothing about the new input's catch-up.
TEST_CASE(sink_reconnected_after_a_blocked_catch_up_catches_up_its_new_input)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto first_producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(first_producer));
    sink->set_ticking(true);
    first_producer->append_output(nullptr, Media::PipelineStatus::Blocked);
    (void)sink->update(MonotonicTime::now());
    EXPECT(!sink->is_catching_up());
    sink->disconnect_input(first_producer);

    auto second_producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(second_producer));
    EXPECT(sink->is_catching_up());
    EXPECT(!sink->frame_is_up_to_date());

    second_producer->append_frame(create_test_frame(*pool, AK::Duration::from_milliseconds(1000), AK::Duration::from_milliseconds(40)));
    (void)sink->update(MonotonicTime::now());
    EXPECT(!sink->is_catching_up());
    EXPECT(sink->frame_is_up_to_date());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}

// Data that has yet to arrive holds a catch-up up indefinitely, so the frame from before it counts as up to date.
TEST_CASE(catch_up_that_waits_for_data_leaves_the_frame_up_to_date)
{
    auto& loop = never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);
    producer->append_frame(create_test_frame(*pool, AK::Duration::zero(), AK::Duration::from_milliseconds(40)));
    (void)sink->update(MonotonicTime::now());
    EXPECT(sink->frame_is_up_to_date());

    sink->set_ticking(false);
    clock->seek(AK::Duration::from_milliseconds(3000));
    sink->set_ticking(true);
    EXPECT(sink->is_catching_up());
    EXPECT(!sink->frame_is_up_to_date());

    producer->append_output(nullptr, Media::PipelineStatus::Blocked);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(!sink->is_catching_up());
    EXPECT(sink->frame_is_up_to_date());
    EXPECT_EQ(sink->current_frame()->timestamp(), AK::Duration::zero());
}

// A sink that stops ticking dispatches its input's statuses directly, which can overtake a status that an earlier update
// left to be dispatched later. The later one must not report the earlier checkpoint after the newer one.
TEST_CASE(deferred_status_does_not_report_an_older_checkpoint)
{
    auto& loop = never_destroyed_event_loop();

    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Vector<Media::PipelineCheckpoint> dispatched_checkpoints;
    sink->set_state_change_handler([&](Media::PipelineStatus, Media::PipelineCheckpoint checkpoint) { dispatched_checkpoints.append(checkpoint); });
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_ticking(true);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    dispatched_checkpoints.clear();

    producer->append_output(nullptr, Media::PipelineStatus::Suspended, 1);
    (void)sink->update(MonotonicTime::now());

    sink->set_ticking(false);
    producer->consume();
    producer->append_output(nullptr, Media::PipelineStatus::Suspended, 2);
    producer->wake();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(dispatched_checkpoints, Vector<Media::PipelineCheckpoint> { 2 });
}
