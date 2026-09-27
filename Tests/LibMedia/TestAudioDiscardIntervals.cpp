/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <AK/Vector.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/AudioDiscardIntervals.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibTest/TestCase.h>

namespace {

constexpr u32 SAMPLE_RATE = 8'000;
constexpr size_t FRAMES_PER_PACKET = 800;

AK::Duration frames(i64 count)
{
    return AK::Duration::from_time_units(count, 1, SAMPLE_RATE);
}

// A mono S16LE packet whose samples are numbered from the given value, so that output can be traced back to it.
Media::CodedFrame numbered_pcm_frame(i16 first_sample_value, AK::Duration decoded_start, AK::Duration leading_discard, AK::Duration trailing_discard)
{
    auto data = MUST(FixedArray<u8>::create(FRAMES_PER_PACKET * sizeof(i16)));
    for (size_t index = 0; index < FRAMES_PER_PACKET; index++) {
        auto sample = static_cast<i16>(first_sample_value + static_cast<i16>(index));
        data[index * 2] = static_cast<u8>(sample & 0xff);
        data[(index * 2) + 1] = static_cast<u8>((sample >> 8) & 0xff);
    }
    auto presentation_timestamp = decoded_start + leading_discard;
    auto duration = frames(FRAMES_PER_PACKET) - leading_discard - trailing_discard;
    Media::CodedFrame frame { Media::CodecID::S16LE, presentation_timestamp, presentation_timestamp, duration, Media::FrameFlags::Keyframe, move(data) };
    frame.set_leading_discard(leading_discard);
    frame.set_trailing_discard(trailing_discard);
    return frame;
}

Media::AudioBlock block_at(AK::Duration start, size_t frame_count)
{
    Media::AudioBlock block;
    block.initialize(Audio::SampleSpecification { SAMPLE_RATE, Audio::ChannelMap::mono() }, start, frame_count);
    return block;
}

i16 sample_value(float sample)
{
    return static_cast<i16>(AK::round_to<i32>(sample * 32768.0f));
}

}

TEST_CASE(blocks_are_limited_to_end_at_discard_boundaries)
{
    Media::AudioDiscardIntervals intervals;
    intervals.add_intervals_for_frame(numbered_pcm_frame(0, AK::Duration::zero(), frames(200), frames(100)));

    // The leading discard ends 200 frames in, and the trailing discard starts 100 frames before the frame's end.
    EXPECT_EQ(intervals.frames_until_next_boundary(AK::Duration::zero(), SAMPLE_RATE, 4096), 200u);
    EXPECT_EQ(intervals.frames_until_next_boundary(frames(200), SAMPLE_RATE, 4096), 500u);
    EXPECT_EQ(intervals.frames_until_next_boundary(frames(700), SAMPLE_RATE, 4096), 100u);
    EXPECT_EQ(intervals.frames_until_next_boundary(frames(800), SAMPLE_RATE, 4096), 4096u);
    EXPECT_EQ(intervals.frames_until_next_boundary(frames(200), SAMPLE_RATE, 64), 64u);
}

TEST_CASE(blocks_inside_discarded_intervals_are_discarded)
{
    Media::AudioDiscardIntervals intervals;
    intervals.add_intervals_for_frame(numbered_pcm_frame(0, AK::Duration::zero(), frames(200), frames(100)));

    EXPECT(intervals.should_discard(block_at(AK::Duration::zero(), 200)));
    EXPECT(!intervals.should_discard(block_at(frames(200), 500)));
    EXPECT(intervals.should_discard(block_at(frames(700), 100)));
    EXPECT(!intervals.should_discard(block_at(frames(800), 100)));
}

TEST_CASE(intervals_behind_the_output_are_forgotten)
{
    Media::AudioDiscardIntervals intervals;
    intervals.add_intervals_for_frame(numbered_pcm_frame(0, AK::Duration::zero(), frames(200), AK::Duration::zero()));

    EXPECT(!intervals.should_discard(block_at(frames(200), 100)));
    // Output never goes backwards without a flush, so the passed interval no longer limits anything.
    EXPECT_EQ(intervals.frames_until_next_boundary(AK::Duration::zero(), SAMPLE_RATE, 4096), 4096u);
}

TEST_CASE(decoder_drops_the_discarded_output_of_its_frames)
{
    auto selection = Media::select_audio_decoder(Media::ParsedCodec { Media::CodecID::S16LE });
    auto decoder = TRY_OR_FAIL(Media::create_audio_decoder(selection, Media::CodecID::S16LE, Audio::SampleSpecification { SAMPLE_RATE, Audio::ChannelMap::mono() }, {}));

    // The first frame's output starts 200 frames before zero, and the second's last 300 frames are not presented.
    auto first_frame = numbered_pcm_frame(0, frames(-200), frames(200), AK::Duration::zero());
    auto second_frame = numbered_pcm_frame(1000, frames(600), AK::Duration::zero(), frames(300));

    Vector<float> samples;
    Optional<AK::Duration> first_block_start;
    auto write_available_blocks = [&] {
        Media::AudioBlock block;
        while (!decoder->write_next_block(block).is_error()) {
            if (!first_block_start.has_value())
                first_block_start = block.media_time_start();
            samples.append(block.channel_data(0).data(), block.frame_count());
        }
    };
    TRY_OR_FAIL(decoder->receive_coded_data(first_frame));
    write_available_blocks();
    TRY_OR_FAIL(decoder->receive_coded_data(second_frame));
    write_available_blocks();
    decoder->signal_end_of_stream();
    write_available_blocks();

    EXPECT_EQ(first_block_start, AK::Duration::zero());
    EXPECT_EQ(samples.size(), 600u + 500u);
    EXPECT_EQ(sample_value(samples.first()), 200);
    EXPECT_EQ(sample_value(samples[599]), 799);
    EXPECT_EQ(sample_value(samples[600]), 1000);
    EXPECT_EQ(sample_value(samples.last()), 1499);
}
