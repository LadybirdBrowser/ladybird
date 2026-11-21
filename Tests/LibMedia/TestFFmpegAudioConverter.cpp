/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibAudio/SampleSpecification.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegChannelLayout.h>
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

TEST_CASE(top_side_speakers_are_ffmpegs_top_side_channels)
{
    auto channel_map = Audio::ChannelMap(
        Audio::Channel::FrontLeft,
        Audio::Channel::FrontRight,
        Audio::Channel::FrontCenter,
        Audio::Channel::LowFrequency,
        Audio::Channel::BackLeft,
        Audio::Channel::BackRight,
        Audio::Channel::TopSideLeft,
        Audio::Channel::TopSideRight);
    auto layout = MUST(Media::FFmpeg::FFmpegChannelLayout::from_channel_map(Media::FFmpeg::FFmpegFunctions::bundled(), channel_map));
    EXPECT_EQ(layout.layout().order, AV_CHANNEL_ORDER_NATIVE);
    if (layout.layout().order != AV_CHANNEL_ORDER_NATIVE)
        return;
    EXPECT_EQ(layout.layout().u.mask, AV_CH_LAYOUT_5POINT1_BACK | AV_CH_TOP_SIDE_LEFT | AV_CH_TOP_SIDE_RIGHT);
    EXPECT_EQ(TRY_OR_FAIL(layout.to_channel_map()), channel_map);
}

TEST_CASE(stereo_converts_into_a_layout_with_top_side_speakers)
{
    auto layout_with_top_side_speakers = Audio::ChannelMap(
        Audio::Channel::FrontLeft,
        Audio::Channel::FrontRight,
        Audio::Channel::FrontCenter,
        Audio::Channel::LowFrequency,
        Audio::Channel::BackLeft,
        Audio::Channel::BackRight,
        Audio::Channel::TopSideLeft,
        Audio::Channel::TopSideRight);
    auto converter = MUST(Media::FFmpeg::FFmpegAudioConverter::try_create());
    MUST(converter->set_output_sample_specification(Audio::SampleSpecification { 48'000, layout_with_top_side_speakers }));

    constexpr size_t frame_count = 480;
    constexpr float front_left_sample = 0.5f;
    constexpr float front_right_sample = -0.25f;
    Media::AudioBlock stereo_block;
    stereo_block.initialize(Audio::SampleSpecification { 48'000, Audio::ChannelMap::stereo() }, 0, frame_count);
    for (size_t frame = 0; frame < frame_count; frame++) {
        stereo_block.set_sample(0, frame, front_left_sample);
        stereo_block.set_sample(1, frame, front_right_sample);
    }
    MUST(converter->push_block(stereo_block));

    Media::AudioBlock converted_block;
    MUST(converter->retrieve_block(converted_block));
    EXPECT_EQ(converted_block.sample_specification().channel_map(), layout_with_top_side_speakers);
    EXPECT_EQ(converted_block.frame_count(), frame_count);
    for (size_t frame = 0; frame < converted_block.frame_count(); frame++) {
        EXPECT_APPROXIMATE(converted_block.sample(0, frame), front_left_sample);
        EXPECT_APPROXIMATE(converted_block.sample(1, frame), front_right_sample);
        for (size_t channel = 2; channel < converted_block.channel_count(); channel++)
            EXPECT_EQ(converted_block.sample(channel, frame), 0.0f);
    }
}

TEST_CASE(a_channel_map_ending_in_one_unknown_channel_becomes_a_custom_layout)
{
    auto channel_map = Audio::ChannelMap(Audio::Channel::FrontLeft, Audio::Channel::FrontRight, Audio::Channel::Unknown);
    auto layout = MUST(Media::FFmpeg::FFmpegChannelLayout::from_channel_map(Media::FFmpeg::FFmpegFunctions::bundled(), channel_map));
    EXPECT_EQ(layout.layout().order, AV_CHANNEL_ORDER_CUSTOM);
    EXPECT_EQ(layout.layout().nb_channels, 3);
    if (layout.layout().order != AV_CHANNEL_ORDER_CUSTOM || layout.layout().nb_channels != 3)
        return;
    EXPECT_EQ(layout.layout().u.map[0].id, AV_CHAN_FRONT_LEFT);
    EXPECT_EQ(layout.layout().u.map[1].id, AV_CHAN_FRONT_RIGHT);
    EXPECT_EQ(layout.layout().u.map[2].id, AV_CHAN_UNKNOWN);
}

TEST_CASE(a_conversion_that_failed_to_set_up_fails_again_when_retried)
{
    auto layout_libswresample_refuses = Audio::ChannelMap(Audio::Channel::FrontLeft, Audio::Channel::FrontRight, Audio::Channel::Unknown);
    auto converter = MUST(Media::FFmpeg::FFmpegAudioConverter::try_create());
    MUST(converter->set_output_sample_specification(Audio::SampleSpecification { 48'000, layout_libswresample_refuses }));

    Media::AudioBlock stereo_block;
    stereo_block.initialize(Audio::SampleSpecification { 48'000, Audio::ChannelMap::stereo() }, 0, 480);
    EXPECT(converter->push_block(stereo_block).is_error());
    EXPECT(converter->push_block(stereo_block).is_error());
}
