/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibAudio/SharedAudioFrameRing.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <math.h>

static constexpr u32 SAMPLE_RATE = 48000;
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

// A second mapping of the same memory, the way the consuming process receives it.
static Audio::SharedAudioFrameRing map_consumer_side(Audio::SharedAudioFrameRing const& producer_ring)
{
    auto buffer = MUST(Core::AnonymousBuffer::create_from_anon_fd(MUST(Core::System::dup(producer_ring.buffer().fd())), producer_ring.buffer().size()));
    return MUST(Audio::SharedAudioFrameRing::create(move(buffer)));
}

TEST_CASE(capacity_is_rounded_up_to_a_power_of_two)
{
    auto ring = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 3));
    EXPECT_EQ(ring.frame_capacity(), 4u);
    EXPECT_EQ(ring.sample_rate(), SAMPLE_RATE);
    EXPECT_EQ(ring.channel_count(), CHANNEL_COUNT);
    EXPECT_EQ(ring.frames_available(), 0u);
    EXPECT_EQ(ring.frames_free(), 4u);

    auto exact_ring = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, 1, 8));
    EXPECT_EQ(exact_ring.frame_capacity(), 8u);
}

TEST_CASE(consumer_mapping_sees_the_header_and_the_frames)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 16));
    auto consumer = map_consumer_side(producer);
    EXPECT_EQ(consumer.frame_capacity(), 16u);
    EXPECT_EQ(consumer.sample_rate(), SAMPLE_RATE);
    EXPECT_EQ(consumer.channel_count(), CHANNEL_COUNT);

    EXPECT_EQ(producer.try_push(make_frames(0, 5)), 5u);
    EXPECT_EQ(consumer.frames_available(), 5u);

    Vector<float> output;
    output.resize(5 * CHANNEL_COUNT);
    EXPECT_EQ(consumer.try_pop(output), 5u);
    for (u64 frame = 0; frame < 5; ++frame)
        for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
            EXPECT_EQ(output[frame * CHANNEL_COUNT + channel], sample_for_frame(frame, channel));
    EXPECT_EQ(producer.frames_free(), 16u);
}

TEST_CASE(fill_and_drain_wraps_around)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 8));
    auto consumer = map_consumer_side(producer);

    EXPECT_EQ(producer.try_push(make_frames(0, 6)), 6u);
    Vector<float> output;
    output.resize(4 * CHANNEL_COUNT);
    EXPECT_EQ(consumer.try_pop(output), 4u);

    // Six more frames fit (two remain), and the write straddles the end of the storage.
    EXPECT_EQ(producer.try_push(make_frames(6, 8)), 6u);
    EXPECT_EQ(producer.frames_free(), 0u);
    EXPECT_EQ(producer.frames_written(), 12u);

    output.resize(8 * CHANNEL_COUNT);
    EXPECT_EQ(consumer.try_pop(output), 8u);
    for (u64 frame = 4; frame < 12; ++frame)
        for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
            EXPECT_EQ(output[(frame - 4) * CHANNEL_COUNT + channel], sample_for_frame(frame, channel));
    EXPECT_EQ(consumer.frames_available(), 0u);
}

TEST_CASE(mixing_pop_scales_and_accumulates)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 8));
    auto consumer = map_consumer_side(producer);

    // Push across the wrap point so the mix walks both chunks.
    Vector<float> output;
    output.resize(6 * CHANNEL_COUNT);
    EXPECT_EQ(producer.try_push(make_frames(0, 6)), 6u);
    EXPECT_EQ(consumer.try_pop(output), 6u);
    EXPECT_EQ(producer.try_push(make_frames(6, 4)), 4u);

    Vector<float> mix;
    mix.resize(6 * CHANNEL_COUNT);
    mix.fill(1.0f);
    EXPECT_EQ(consumer.pop_mixing_into(mix, 0.5f), 4u);
    for (u64 frame = 6; frame < 10; ++frame)
        for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel)
            EXPECT_EQ(mix[(frame - 6) * CHANNEL_COUNT + channel], 1.0f + sample_for_frame(frame, channel) * 0.5f);
    // Frames the ring could not supply are left alone.
    for (size_t i = 4 * CHANNEL_COUNT; i < mix.size(); ++i)
        EXPECT_EQ(mix[i], 1.0f);
    EXPECT_EQ(consumer.frames_available(), 0u);
}

TEST_CASE(mixing_pop_wraps_with_six_channels)
{
    // Eight frames of six channels hold 48 samples, which is not a power of two.
    static constexpr u32 channel_count = 6;
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, channel_count, 8));
    auto consumer = map_consumer_side(producer);
    // Kept within the magnitude a mixing pop lets through.
    auto stamp = [](u64 frame, u32 channel) { return static_cast<float>(frame) + (static_cast<float>(channel) * 0.125f); };
    auto push_frames = [&](u64 first_frame, size_t frame_count) {
        Vector<float> samples;
        for (u64 frame = first_frame; frame < first_frame + frame_count; ++frame)
            for (u32 channel = 0; channel < channel_count; ++channel)
                samples.append(stamp(frame, channel));
        EXPECT_EQ(producer.try_push(samples), frame_count);
    };

    Vector<float> output;
    output.resize(5 * channel_count);
    push_frames(0, 5);
    EXPECT_EQ(consumer.try_pop(output), 5u);
    push_frames(5, 8);

    Vector<float> mix;
    mix.resize(8 * channel_count);
    mix.fill(0.0f);
    EXPECT_EQ(consumer.pop_mixing_into(mix, 1.0f), 8u);
    for (u64 frame = 5; frame < 13; ++frame)
        for (u32 channel = 0; channel < channel_count; ++channel)
            EXPECT_EQ(mix[(frame - 5) * channel_count + channel], stamp(frame, channel));
}

TEST_CASE(mixing_pop_keeps_the_mix_finite)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 8));
    auto consumer = map_consumer_side(producer);
    auto huge = NumericLimits<float>::max();
    EXPECT_EQ(producer.try_push(Vector<float> { NAN, INFINITY, -INFINITY, 0.5f, huge, -huge, huge, -huge }), 4u);

    // Non-finite samples add nothing, and the rest pass through.
    Vector<float> mix;
    mix.resize(2 * CHANNEL_COUNT);
    mix.fill(0.0f);
    EXPECT_EQ(consumer.pop_mixing_into(mix, 1.0f), 2u);
    EXPECT_EQ(mix[0], 0.0f);
    EXPECT_EQ(mix[1], 0.0f);
    EXPECT_EQ(mix[2], 0.0f);
    EXPECT_EQ(mix[3], 0.5f);

    // Huge samples are bounded, so adding two of them up stays finite.
    mix.resize(CHANNEL_COUNT);
    mix.fill(0.0f);
    EXPECT_EQ(consumer.pop_mixing_into(mix, 1.0f), 1u);
    EXPECT_EQ(consumer.pop_mixing_into(mix, 1.0f), 1u);
    EXPECT(isfinite(mix[0]) && mix[0] > 0.0f);
    EXPECT(isfinite(mix[1]) && mix[1] < 0.0f);
}

TEST_CASE(discard_all_empties_the_ring)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 8));
    auto consumer = map_consumer_side(producer);
    EXPECT_EQ(producer.try_push(make_frames(0, 7)), 7u);
    consumer.discard_all();
    EXPECT_EQ(consumer.frames_available(), 0u);
    EXPECT_EQ(producer.frames_free(), 8u);
    EXPECT_EQ(producer.frames_written(), 7u);
}

TEST_CASE(played_frames_follow_the_consumption_anchor)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 1024));
    auto consumer = map_consumer_side(producer);

    // Nothing published yet: nothing played.
    EXPECT_EQ(producer.estimate_frames_played(1'000'000), 0u);

    EXPECT_EQ(producer.try_push(make_frames(0, 600)), 600u);
    Vector<float> output;
    output.resize(480 * CHANNEL_COUNT);
    EXPECT_EQ(consumer.try_pop(output), 480u);
    // The device will have played those 480 frames at 10 ms.
    consumer.publish_consumption(10'000'000, true);

    // 1 ms before that, 48 of them are still in the device.
    EXPECT_EQ(producer.estimate_frames_played(9'000'000), 432u);
    // At the anchor instant, exactly the consumed frames have played.
    EXPECT_EQ(producer.estimate_frames_played(10'000'000), 480u);
    // Extrapolation at the sample rate: 1 ms later is 48 more frames.
    EXPECT_EQ(producer.estimate_frames_played(11'000'000), 528u);
    // Never past what was written.
    EXPECT_EQ(producer.estimate_frames_played(1'000'000'000), 600u);
    // Never decreasing, even if asked about an earlier instant afterwards.
    EXPECT_EQ(producer.estimate_frames_played(10'500'000), 600u);

    // A non-playing anchor still plays out what the device holds, then freezes at its consumed count.
    auto fresh_producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 1024));
    auto fresh_consumer = map_consumer_side(fresh_producer);
    EXPECT_EQ(fresh_producer.try_push(make_frames(0, 600)), 600u);
    EXPECT_EQ(fresh_consumer.try_pop(output), 480u);
    fresh_consumer.publish_consumption(10'000'000, false);
    EXPECT_EQ(fresh_producer.estimate_frames_played(5'000'000), 240u);
    EXPECT_EQ(fresh_producer.estimate_frames_played(20'000'000), 480u);

    // A device holding more than was ever consumed from this ring has played none of it.
    auto late_producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 1024));
    auto late_consumer = map_consumer_side(late_producer);
    EXPECT_EQ(late_producer.try_push(make_frames(0, 48)), 48u);
    EXPECT_EQ(late_consumer.try_pop(output), 48u);
    late_consumer.publish_consumption(10'000'000, true);
    EXPECT_EQ(late_producer.estimate_frames_played(1'000'000), 0u);
}

TEST_CASE(rejects_buffers_that_do_not_match_their_header)
{
    auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 64));

    // A mapping that claims fewer bytes than the header's capacity needs.
    auto truncated = MUST(Core::AnonymousBuffer::create_from_anon_fd(MUST(Core::System::dup(producer.buffer().fd())), 512));
    EXPECT(Audio::SharedAudioFrameRing::create(move(truncated)).is_error());

    // A buffer with no header to speak of.
    auto zeroed = MUST(Core::AnonymousBuffer::create_with_size(4096));
    EXPECT(Audio::SharedAudioFrameRing::create(move(zeroed)).is_error());

    // A power-of-two capacity whose size in bytes wraps around to nothing.
    auto overflowing = producer.buffer();
    reinterpret_cast<Audio::AudioFrameRingHeader*>(overflowing.data<void>())->frame_capacity = 1ull << 61;
    EXPECT(Audio::SharedAudioFrameRing::create(move(overflowing)).is_error());
}

TEST_CASE(concurrent_producer_and_consumer_preserve_frame_order)
{
    static constexpr u64 TOTAL_FRAMES = 200'000;
    IGNORE_USE_IN_ESCAPING_LAMBDA auto producer = MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, 256));
    auto consumer = map_consumer_side(producer);

    auto producer_thread = Threading::Thread::construct("RingProducer"sv, [&]() -> intptr_t {
        u64 next_frame = 0;
        while (next_frame < TOTAL_FRAMES) {
            auto frame_count = min<u64>(37, TOTAL_FRAMES - next_frame);
            auto frames = make_frames(next_frame, frame_count);
            size_t pushed_frames = 0;
            while (pushed_frames < frame_count) {
                auto remaining = ReadonlySpan<float>(frames).slice(pushed_frames * CHANNEL_COUNT);
                pushed_frames += producer.try_push(remaining);
            }
            next_frame += frame_count;
        }
        return 0;
    });
    producer_thread->start();

    Vector<float> output;
    output.resize(53 * CHANNEL_COUNT);
    u64 next_expected_frame = 0;
    bool all_in_order = true;
    while (next_expected_frame < TOTAL_FRAMES) {
        auto popped_frames = consumer.try_pop(output);
        for (size_t frame = 0; frame < popped_frames; ++frame) {
            for (u32 channel = 0; channel < CHANNEL_COUNT; ++channel) {
                if (output[frame * CHANNEL_COUNT + channel] != sample_for_frame(next_expected_frame, channel))
                    all_in_order = false;
            }
            next_expected_frame++;
        }
    }
    (void)producer_thread->join();
    EXPECT(all_in_order);
    EXPECT_EQ(consumer.frames_available(), 0u);
}
