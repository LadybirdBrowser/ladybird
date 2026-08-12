/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

static constexpr u32 CHANNEL_COUNT = 2;

// Encodes a float32-exact stamp for each sample of a frame, so that any reordered, torn,
// or repeated frame is detectable.
static float sample_for_frame(u64 frame_index, u32 channel)
{
    return static_cast<float>(frame_index % (1u << 20)) + (static_cast<float>(channel) * 0.25f);
}

static Vector<float> make_frames(u64 first_frame_index, size_t frame_count)
{
    Vector<float> samples;
    samples.ensure_capacity(frame_count * CHANNEL_COUNT);
    for (u64 frame = first_frame_index; frame < first_frame_index + frame_count; ++frame)
        for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
            samples.append(sample_for_frame(frame, channel));
    return samples;
}

TEST_CASE(capacity_is_rounded_up_to_a_power_of_two)
{
    auto ring = make_ref_counted<Media::SpscAudioFrameRing>(3, CHANNEL_COUNT);
    EXPECT_EQ(ring->frame_capacity(), 4u);
    EXPECT_EQ(ring->channel_count(), CHANNEL_COUNT);
    EXPECT_EQ(ring->frames_available(), 0u);
    EXPECT_EQ(ring->frames_free(), 4u);

    auto exact_ring = make_ref_counted<Media::SpscAudioFrameRing>(8, 1u);
    EXPECT_EQ(exact_ring->frame_capacity(), 8u);
}

TEST_CASE(fill_and_drain)
{
    auto ring = make_ref_counted<Media::SpscAudioFrameRing>(8, CHANNEL_COUNT);

    auto input = make_frames(0, 8);
    EXPECT_EQ(ring->try_push(input), 8u);
    EXPECT_EQ(ring->frames_available(), 8u);
    EXPECT_EQ(ring->frames_free(), 0u);

    EXPECT_EQ(ring->try_push(input), 0u);

    Vector<float> output;
    output.resize(8 * CHANNEL_COUNT);
    EXPECT_EQ(ring->try_pop(output), 8u);
    EXPECT_EQ(ring->frames_available(), 0u);
    EXPECT_EQ(ring->frames_free(), 8u);
    for (size_t i = 0; i < output.size(); ++i)
        EXPECT_EQ(output[i], input[i]);

    EXPECT_EQ(ring->try_pop(output), 0u);
}

TEST_CASE(partial_push_and_pop)
{
    auto ring = make_ref_counted<Media::SpscAudioFrameRing>(4, CHANNEL_COUNT);

    auto three_frames = make_frames(0, 3);
    EXPECT_EQ(ring->try_push(three_frames), 3u);
    EXPECT_EQ(ring->try_push(three_frames), 1u);
    EXPECT_EQ(ring->frames_free(), 0u);

    Vector<float> output;
    output.resize(8 * CHANNEL_COUNT);
    EXPECT_EQ(ring->try_pop(output), 4u);

    for (u64 frame = 0; frame < 3; ++frame)
        for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
            EXPECT_EQ(output[frame * CHANNEL_COUNT + channel], sample_for_frame(frame, channel));
    for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
        EXPECT_EQ(output[3 * CHANNEL_COUNT + channel], sample_for_frame(0, channel));
}

TEST_CASE(wraparound_preserves_frame_order)
{
    auto ring = make_ref_counted<Media::SpscAudioFrameRing>(8, CHANNEL_COUNT);

    // Push and pop 5 frames at a time; 5 does not divide 8, so the writes and reads keep
    // crossing the wraparound boundary at different offsets.
    u64 next_frame = 0;
    Vector<float> output;
    output.resize(5 * CHANNEL_COUNT);
    for (int iteration = 0; iteration < 100; ++iteration) {
        EXPECT_EQ(ring->try_push(make_frames(next_frame, 5)), 5u);
        EXPECT_EQ(ring->try_pop(output), 5u);
        for (u64 i = 0; i < 5; ++i)
            for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
                EXPECT_EQ(output[i * CHANNEL_COUNT + channel], sample_for_frame(next_frame + i, channel));
        next_frame += 5;
    }
}

TEST_CASE(two_thread_stress)
{
    auto ring = make_ref_counted<Media::SpscAudioFrameRing>(64, CHANNEL_COUNT);

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> producer_done { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> frames_pushed { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> frames_popped { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> mismatch_count { 0 };

    auto producer_thread = Threading::Thread::construct("RingProducer"sv, [ring, &producer_done, &frames_pushed]() {
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(1);
        u64 next_frame = 0;
        size_t chunk_frames = 1;
        while (MonotonicTime::now() < deadline) {
            auto chunk = make_frames(next_frame, chunk_frames);

            // Retry the frames that did not fit, so the consumer sees an unbroken sequence.
            size_t frames_written = 0;
            while (frames_written < chunk_frames)
                frames_written += ring->try_push(chunk.span().slice(frames_written * CHANNEL_COUNT));

            next_frame += chunk_frames;
            chunk_frames = (chunk_frames % 17) + 1;
        }
        frames_pushed.store(next_frame);
        producer_done.store(true);
        return 0;
    });

    auto consumer_thread = Threading::Thread::construct("RingConsumer"sv, [ring, &producer_done, &frames_popped, &mismatch_count]() {
        u64 next_frame = 0;
        u64 mismatches = 0;
        size_t chunk_frames = 3;
        Vector<float> buffer;
        buffer.resize(17 * CHANNEL_COUNT);
        while (true) {
            auto frames_read = ring->try_pop(buffer.span().slice(0, chunk_frames * CHANNEL_COUNT));
            if (frames_read == 0) {
                if (producer_done.load() && ring->frames_available() == 0)
                    break;
                continue;
            }
            for (u64 i = 0; i < frames_read; ++i)
                for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
                    if (buffer[i * CHANNEL_COUNT + channel] != sample_for_frame(next_frame + i, channel))
                        ++mismatches;
            next_frame += frames_read;
            chunk_frames = (chunk_frames % 13) + 1;
        }
        frames_popped.store(next_frame);
        mismatch_count.store(mismatches);
        return 0;
    });

    producer_thread->start();
    consumer_thread->start();
    (void)producer_thread->join();
    (void)consumer_thread->join();

    EXPECT(frames_pushed.load() > 0u);
    EXPECT_EQ(frames_popped.load(), frames_pushed.load());
    EXPECT_EQ(mismatch_count.load(), 0u);
    EXPECT_EQ(ring->frames_available(), 0u);
}
