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
