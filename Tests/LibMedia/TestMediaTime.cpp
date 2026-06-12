/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibMedia/MediaTime.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

static constexpr u32 SAMPLE_RATE = 48000;
static constexpr i64 FRAMES_PER_BLOCK = 1000;

static Media::AudioBlockTiming make_timing(i64 block_index)
{
    Media::AudioBlockTiming timing;
    timing.initialize(block_index * FRAMES_PER_BLOCK, SAMPLE_RATE, FRAMES_PER_BLOCK);
    return timing;
}

static AK::Duration media_time_for_frame(i64 frame_index)
{
    return AK::Duration::from_time_units(frame_index, 1, SAMPLE_RATE);
}

TEST_CASE(fresh_clock_reads_zero)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    EXPECT_EQ(reader.current_time(MonotonicTime::now()), AK::Duration::zero());
}

TEST_CASE(audio_anchor_refined_by_timing_ring)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    for (i64 block = 0; block < 4; ++block)
        writer.timing_ring().enqueue(make_timing(block));
    writer.publish_audio_anchor(base, 0, AK::Duration::zero(), SAMPLE_RATE, true);

    auto at_10ms = reader.current_time(base + AK::Duration::from_milliseconds(10));
    EXPECT_EQ(at_10ms, media_time_for_frame(480));

    auto at_50ms = reader.current_time(base + AK::Duration::from_milliseconds(50));
    EXPECT_EQ(at_50ms, media_time_for_frame(2400));
}

TEST_CASE(time_clamps_at_ring_end)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    for (i64 block = 0; block < 4; ++block)
        writer.timing_ring().enqueue(make_timing(block));
    writer.publish_audio_anchor(base, 0, AK::Duration::zero(), SAMPLE_RATE, true);

    auto far_future = reader.current_time(base + AK::Duration::from_seconds(10));
    EXPECT_EQ(far_future, make_timing(3).media_time_end());
}

TEST_CASE(paused_anchor_freezes_time)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    for (i64 block = 0; block < 4; ++block)
        writer.timing_ring().enqueue(make_timing(block));
    writer.publish_audio_anchor(base, 2400, media_time_for_frame(2400), SAMPLE_RATE, false);

    EXPECT_EQ(reader.current_time(base), media_time_for_frame(2400));
    EXPECT_EQ(reader.current_time(base + AK::Duration::from_seconds(5)), media_time_for_frame(2400));
}

TEST_CASE(empty_ring_falls_back_to_anchor_without_extrapolating)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    auto anchor_time = AK::Duration::from_seconds(10);
    writer.publish_audio_anchor(base, 480000, anchor_time, SAMPLE_RATE, true);

    EXPECT_EQ(reader.current_time(base + AK::Duration::from_seconds(5)), anchor_time);
}

TEST_CASE(hard_seek_lowers_time_across_epochs)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    for (i64 block = 0; block < 4; ++block)
        writer.timing_ring().enqueue(make_timing(block));
    writer.publish_audio_anchor(base, 0, AK::Duration::zero(), SAMPLE_RATE, true);

    // Raise the reader's floor within the first epoch.
    auto before_seek = reader.current_time(base + AK::Duration::from_milliseconds(50));
    EXPECT_EQ(before_seek, media_time_for_frame(2400));

    auto target = AK::Duration::from_milliseconds(10);
    writer.seek(target);

    // The seek window reads the target at any time, below the previous floor.
    EXPECT_EQ(reader.current_time(base + AK::Duration::from_seconds(1)), target);
    EXPECT_EQ(reader.current_time(base + AK::Duration::from_seconds(2)), target);
}

TEST_CASE(floor_is_monotonic_within_an_epoch)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    writer.publish_audio_anchor(base, 4800, AK::Duration::from_milliseconds(100), SAMPLE_RATE, false);
    EXPECT_EQ(reader.current_time(base), AK::Duration::from_milliseconds(100));

    // An anchor that regresses without a seek must not move time backwards.
    writer.publish_audio_anchor(base, 4320, AK::Duration::from_milliseconds(90), SAMPLE_RATE, false);
    EXPECT_EQ(reader.current_time(base), AK::Duration::from_milliseconds(100));
}

TEST_CASE(monotonic_mode_scales_by_playback_rate)
{
    auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto base = MonotonicTime::now();

    writer.publish_monotonic_anchor(base, AK::Duration::from_seconds(1), 2.0f, true);
    EXPECT_EQ(reader.current_time(base + AK::Duration::from_milliseconds(500)), AK::Duration::from_seconds(2));

    // A rate change re-anchors at the current media time.
    writer.publish_monotonic_anchor(base + AK::Duration::from_milliseconds(500), AK::Duration::from_seconds(2), 1.0f, true);
    EXPECT_EQ(reader.current_time(base + AK::Duration::from_milliseconds(1500)), AK::Duration::from_seconds(3));
}

TEST_CASE(clock_is_readable_through_a_second_mapping)
{
    auto writer = MUST(Media::MediaTimeWriter::create());

    auto reader_fd = MUST(Core::System::dup(writer.buffer().fd()));
    auto reader_buffer = MUST(Core::AnonymousBuffer::create_from_anon_fd(reader_fd, sizeof(Media::MediaTimeData)));
    auto reader = MUST(Media::MediaTimeReader::create(reader_buffer));

    auto base = MonotonicTime::now();
    writer.publish_monotonic_anchor(base, AK::Duration::from_seconds(7), 1.0f, false);
    EXPECT_EQ(reader.current_time(base + AK::Duration::from_seconds(1)), AK::Duration::from_seconds(7));
}

TEST_CASE(torn_write_returns_the_readers_floor)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Media::MediaTimeData)));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto& data = *new (buffer.data<void>()) Media::MediaTimeData;
    auto reader = MUST(Media::MediaTimeReader::create(buffer));
    auto base = MonotonicTime::now();

    data.record.write([&](TearableAtomic<Media::MediaTimeRecord>& slot) {
        auto record = slot.load_relaxed();
        record.mode = Media::MediaTimeMode::MonotonicDriven;
        record.anchor_media_time_nanoseconds = AK::Duration::from_seconds(5).to_nanoseconds();
        slot.store_relaxed(record);
    });
    EXPECT_EQ(reader.current_time(base), AK::Duration::from_seconds(5));

    // While a write is in progress (an odd version), the reader stays on its last good value.
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> write_in_progress { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> finish_write { false };
    auto stuck_writer = Threading::Thread::construct("StuckClockWriter"sv, [&data, &write_in_progress, &finish_write] {
        data.record.write([&](TearableAtomic<Media::MediaTimeRecord>&) {
            write_in_progress.store(true);
            while (!finish_write.load()) { }
        });
        return 0;
    });
    stuck_writer->start();
    while (!write_in_progress.load()) { }
    EXPECT_EQ(reader.current_time(base), AK::Duration::from_seconds(5));
    finish_write.store(true);
    (void)stuck_writer->join();
}

TEST_CASE(concurrent_writer_and_reader)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto writer = MUST(Media::MediaTimeWriter::create());
    auto reader = MUST(Media::MediaTimeReader::create(writer.buffer()));
    auto const anchor_count = 100'000;
    auto base = MonotonicTime::now();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> writer_done { false };

    auto writer_thread = Threading::Thread::construct("ClockWriter"sv, [&writer, &writer_done, base]() {
        for (i64 i = 1; i <= anchor_count; ++i)
            writer.publish_monotonic_anchor(base, AK::Duration::from_microseconds(i), 1.0f, false);
        writer_done.store(true);
        return 0;
    });
    writer_thread->start();

    // Paused anchors only ever increase within this epoch, so reads must be monotonic, and a
    // torn read would either fail the version check or get caught by the final exact match.
    auto last_time = AK::Duration::zero();
    while (!writer_done.load()) {
        auto time = reader.current_time(base);
        EXPECT(time >= last_time);
        last_time = time;
    }
    (void)writer_thread->join();

    EXPECT_EQ(reader.current_time(base), AK::Duration::from_microseconds(anchor_count));
}
