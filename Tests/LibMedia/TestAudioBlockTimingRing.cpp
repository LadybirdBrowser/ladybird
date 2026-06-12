/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibMedia/AudioBlockTimingRing.h>
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

TEST_CASE(empty_ring_has_no_timings)
{
    Media::AudioBlockTimingRing ring;
    EXPECT(!ring.latest_timing().has_value());
    EXPECT(!ring.find_timing_for_frame_index(0).has_value());
}

TEST_CASE(find_timing_for_frame_index)
{
    Media::AudioBlockTimingRing ring;
    for (i64 block = 0; block < 4; ++block)
        ring.enqueue(make_timing(block));

    // An index within a block finds that block.
    for (i64 block = 0; block < 4; ++block) {
        auto timing = ring.find_timing_for_frame_index((block * FRAMES_PER_BLOCK) + (FRAMES_PER_BLOCK / 2));
        EXPECT(timing.has_value());
        EXPECT_EQ(timing->first_frame_index(), block * FRAMES_PER_BLOCK);
    }

    // An index past the newest block finds the newest block.
    auto past_end = ring.find_timing_for_frame_index(10 * FRAMES_PER_BLOCK);
    EXPECT(past_end.has_value());
    EXPECT_EQ(past_end->first_frame_index(), 3 * FRAMES_PER_BLOCK);

    // An index before the oldest block falls back to the oldest block.
    auto before_start = ring.find_timing_for_frame_index(-FRAMES_PER_BLOCK);
    EXPECT(before_start.has_value());
    EXPECT_EQ(before_start->first_frame_index(), 0);

    auto latest = ring.latest_timing();
    EXPECT(latest.has_value());
    EXPECT_EQ(latest->first_frame_index(), 3 * FRAMES_PER_BLOCK);
}

TEST_CASE(clear_invalidates_timings)
{
    Media::AudioBlockTimingRing ring;
    for (i64 block = 0; block < 4; ++block)
        ring.enqueue(make_timing(block));

    ring.clear();
    EXPECT(!ring.latest_timing().has_value());
    EXPECT(!ring.find_timing_for_frame_index(FRAMES_PER_BLOCK).has_value());

    ring.enqueue(make_timing(20));
    auto timing = ring.latest_timing();
    EXPECT(timing.has_value());
    EXPECT_EQ(timing->first_frame_index(), 20 * FRAMES_PER_BLOCK);
}

TEST_CASE(wraparound_keeps_only_newest_timings)
{
    Media::AudioBlockTimingRing ring;
    auto const block_count = static_cast<i64>(Media::AudioBlockTimingRing::capacity) * 2;
    for (i64 block = 0; block < block_count; ++block)
        ring.enqueue(make_timing(block));

    // Timings that were overwritten fall back to the oldest retained block.
    auto old_timing = ring.find_timing_for_frame_index(0);
    EXPECT(old_timing.has_value());
    auto const oldest_retained_block = block_count - static_cast<i64>(Media::AudioBlockTimingRing::capacity);
    EXPECT_EQ(old_timing->first_frame_index(), oldest_retained_block * FRAMES_PER_BLOCK);

    auto latest = ring.latest_timing();
    EXPECT(latest.has_value());
    EXPECT_EQ(latest->first_frame_index(), (block_count - 1) * FRAMES_PER_BLOCK);
}

// The ring is designed to be placed in shared memory and read from another mapping of the
// same memory, as another process would.
TEST_CASE(ring_is_readable_through_a_second_mapping)
{
    auto writer_buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Media::AudioBlockTimingRing)));
    auto& writer_ring = *new (writer_buffer.data<void>()) Media::AudioBlockTimingRing;

    auto reader_fd = MUST(Core::System::dup(writer_buffer.fd()));
    auto reader_buffer = MUST(Core::AnonymousBuffer::create_from_anon_fd(reader_fd, sizeof(Media::AudioBlockTimingRing)));
    auto& reader_ring = *reinterpret_cast<Media::AudioBlockTimingRing*>(reader_buffer.data<void>());
    EXPECT_NE(static_cast<void*>(&writer_ring), static_cast<void*>(&reader_ring));

    for (i64 block = 0; block < 4; ++block)
        writer_ring.enqueue(make_timing(block));

    auto timing = reader_ring.find_timing_for_frame_index(FRAMES_PER_BLOCK + 1);
    EXPECT(timing.has_value());
    EXPECT_EQ(timing->first_frame_index(), FRAMES_PER_BLOCK);

    writer_ring.clear();
    EXPECT(!reader_ring.latest_timing().has_value());
}

TEST_CASE(concurrent_reader_never_sees_torn_timings)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Media::AudioBlockTimingRing ring;
    auto const block_count = 100'000;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> writer_done { false };

    auto reader_thread = Threading::Thread::construct("TimingReader"sv, [&ring, &writer_done]() {
        i64 last_seen_block = -1;
        while (!writer_done.load() || last_seen_block < 0) {
            auto verify_consistency = [&](Media::AudioBlockTiming const& timing) {
                // Every enqueued timing derives its media time from its frame index, so any
                // torn read mixing two records will fail this linkage.
                EXPECT_EQ(timing.media_time_start(), AK::Duration::from_time_units(timing.first_frame_index(), 1, SAMPLE_RATE));
                EXPECT_EQ(timing.frame_count(), FRAMES_PER_BLOCK);
            };

            auto latest = ring.latest_timing();
            if (!latest.has_value())
                continue;
            verify_consistency(latest.value());
            last_seen_block = latest->first_frame_index() / FRAMES_PER_BLOCK;

            auto found = ring.find_timing_for_frame_index(latest->first_frame_index() - (FRAMES_PER_BLOCK * 4));
            if (found.has_value())
                verify_consistency(found.value());
        }
        return 0;
    });
    reader_thread->start();

    for (i64 block = 0; block < block_count; ++block)
        ring.enqueue(make_timing(block));
    writer_done.store(true);

    (void)reader_thread->join();

    auto latest = ring.latest_timing();
    EXPECT(latest.has_value());
    EXPECT_EQ(latest->first_frame_index(), (block_count - 1) * FRAMES_PER_BLOCK);
}
