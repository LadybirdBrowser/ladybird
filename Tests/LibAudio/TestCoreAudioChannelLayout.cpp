/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibAudio/CoreAudioChannelLayout.h>
#include <LibTest/TestCase.h>

#include <CoreAudioTypes/CoreAudioBaseTypes.h>

TEST_CASE(layout_with_top_middle_speakers_names_every_channel)
{
    Array<AudioChannelLabel, 8> labels {
        kAudioChannelLabel_Left,
        kAudioChannelLabel_Right,
        kAudioChannelLabel_Center,
        kAudioChannelLabel_LFEScreen,
        kAudioChannelLabel_LeftSurround,
        kAudioChannelLabel_RightSurround,
        kAudioChannelLabel_LeftTopMiddle,
        kAudioChannelLabel_RightTopMiddle,
    };
    constexpr size_t layout_size = offsetof(AudioChannelLayout, mChannelDescriptions) + labels.size() * sizeof(AudioChannelDescription);
    alignas(AudioChannelLayout) Array<u8, layout_size> layout_storage {};
    auto& layout = *reinterpret_cast<AudioChannelLayout*>(layout_storage.data());
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    layout.mNumberChannelDescriptions = labels.size();
    // The SDK declares one description, so the rest are reached through the storage rather than past that array.
    auto* descriptions = reinterpret_cast<AudioChannelDescription*>(layout_storage.data() + offsetof(AudioChannelLayout, mChannelDescriptions));
    for (size_t i = 0; i < labels.size(); i++)
        descriptions[i].mChannelLabel = labels[i];

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, layout_size));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::FrontCenter,
            Audio::Channel::LowFrequency,
            Audio::Channel::BackLeft,
            Audio::Channel::BackRight,
            Audio::Channel::TopSideLeft,
            Audio::Channel::TopSideRight));
}

TEST_CASE(channel_bitmap_with_top_middle_speakers_names_every_channel)
{
    AudioChannelLayout layout {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
    layout.mChannelBitmap = kAudioChannelBit_Left | kAudioChannelBit_Right | kAudioChannelBit_LeftTopMiddle | kAudioChannelBit_RightTopMiddle;

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, sizeof(layout)));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::TopSideLeft,
            Audio::Channel::TopSideRight));
}

TEST_CASE(channel_bitmap_names_its_channels_in_bit_order)
{
    AudioChannelLayout layout {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
    layout.mChannelBitmap = kAudioChannelBit_Left | kAudioChannelBit_Right | kAudioChannelBit_TopCenterSurround | kAudioChannelBit_LeftTopFront | kAudioChannelBit_TopBackLeft;

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, sizeof(layout)));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::TopCenter,
            Audio::Channel::TopFrontLeft,
            Audio::Channel::TopBackLeft));
}

TEST_CASE(channel_bitmap_names_bits_without_a_position_as_unknown_channels)
{
    AudioChannelLayout layout {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
    constexpr u32 bit_without_a_position = 1u << 27;
    layout.mChannelBitmap = kAudioChannelBit_Left | kAudioChannelBit_Right | static_cast<AudioChannelBitmap>(bit_without_a_position);

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, sizeof(layout)));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::Unknown));
}

TEST_CASE(atmos_layout_names_its_top_rear_speakers)
{
    AudioChannelLayout layout {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_Atmos_5_1_4;

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, sizeof(layout)));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::FrontCenter,
            Audio::Channel::LowFrequency,
            Audio::Channel::BackLeft,
            Audio::Channel::BackRight,
            Audio::Channel::TopFrontLeft,
            Audio::Channel::TopFrontRight,
            Audio::Channel::TopBackLeft,
            Audio::Channel::TopBackRight));
}

TEST_CASE(channel_bitmap_names_its_top_rear_speakers)
{
    AudioChannelLayout layout {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
    layout.mChannelBitmap = kAudioChannelBit_Left | kAudioChannelBit_Right | kAudioChannelBit_LeftTopRear | kAudioChannelBit_CenterTopRear | kAudioChannelBit_RightTopRear;

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, sizeof(layout)));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::TopBackLeft,
            Audio::Channel::TopBackCenter,
            Audio::Channel::TopBackRight));
}

TEST_CASE(audio_unit_7_1_layout_puts_its_side_channels_before_its_back_channels)
{
    AudioChannelLayout layout {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_AudioUnit_7_1;

    auto channel_map = MUST(Audio::core_audio_channel_layout_to_channel_map(layout, sizeof(layout)));
    EXPECT_EQ(channel_map,
        Audio::ChannelMap(
            Audio::Channel::FrontLeft,
            Audio::Channel::FrontRight,
            Audio::Channel::FrontCenter,
            Audio::Channel::LowFrequency,
            Audio::Channel::SideLeft,
            Audio::Channel::SideRight,
            Audio::Channel::BackLeft,
            Audio::Channel::BackRight));
}
