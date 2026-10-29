/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibAudio/SampleSpecification.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibMediaCodecs/Limits.h>
#include <LibTest/TestCase.h>

TEST_CASE(blocks_outside_the_sample_rate_range_are_refused)
{
    auto push_block_at_sample_rate = [](u32 sample_rate) {
        auto converter = MUST(Media::FFmpeg::FFmpegAudioConverter::try_create());
        MUST(converter->set_output_sample_specification(Audio::SampleSpecification { 48'000, Audio::ChannelMap::stereo() }));
        Media::AudioBlock block;
        block.initialize(Audio::SampleSpecification { sample_rate, Audio::ChannelMap::stereo() }, 0, 1024);
        return converter->push_block(block);
    };

    EXPECT(!push_block_at_sample_rate(Media::MIN_AUDIO_SAMPLE_RATE).is_error());
    EXPECT(!push_block_at_sample_rate(Media::MAX_AUDIO_SAMPLE_RATE).is_error());
    EXPECT(push_block_at_sample_rate(1).is_error());
    EXPECT(push_block_at_sample_rate(Media::MAX_AUDIO_SAMPLE_RATE + 1).is_error());
}

TEST_CASE(output_specifications_outside_the_sample_rate_range_are_refused)
{
    auto set_output_sample_rate = [](u32 sample_rate) {
        auto converter = MUST(Media::FFmpeg::FFmpegAudioConverter::try_create());
        return converter->set_output_sample_specification(Audio::SampleSpecification { sample_rate, Audio::ChannelMap::stereo() });
    };

    EXPECT(!set_output_sample_rate(Media::MIN_AUDIO_SAMPLE_RATE).is_error());
    EXPECT(!set_output_sample_rate(Media::MAX_AUDIO_SAMPLE_RATE).is_error());
    EXPECT(set_output_sample_rate(1).is_error());
    EXPECT(set_output_sample_rate(Media::MAX_AUDIO_SAMPLE_RATE + 1).is_error());
}
