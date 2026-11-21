/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibAudio/CoreAudioChannelLayout.h>

#include <AudioToolbox/AudioFormat.h>

namespace Audio {

static void check_core_audio_channel_layout_size(AudioChannelLayout const& layout, u32 size)
{
    auto minimum_layout_size = Checked(layout.mNumberChannelDescriptions);
    minimum_layout_size *= sizeof(layout.mChannelDescriptions[0]);
    minimum_layout_size += offsetof(AudioChannelLayout, mChannelDescriptions);
    VERIFY(size >= minimum_layout_size.value());
}

#define ENUMERATE_CHANNEL_POSITIONS(C)          \
    C(Left, Channel::FrontLeft)                 \
    C(Right, Channel::FrontRight)               \
    C(Center, Channel::FrontCenter)             \
    C(LFEScreen, Channel::LowFrequency)         \
    C(LeftSurround, Channel::BackLeft)          \
    C(RightSurround, Channel::BackRight)        \
    C(LeftCenter, Channel::FrontLeftOfCenter)   \
    C(RightCenter, Channel::FrontRightOfCenter) \
    C(CenterSurround, Channel::BackCenter)      \
    C(LeftSurroundDirect, Channel::SideLeft)    \
    C(RightSurroundDirect, Channel::SideRight)  \
    C(TopCenterSurround, Channel::TopCenter)    \
    C(TopBackLeft, Channel::TopBackLeft)        \
    C(TopBackCenter, Channel::TopBackCenter)    \
    C(TopBackRight, Channel::TopBackRight)      \
    C(LeftTopFront, Channel::TopFrontLeft)      \
    C(CenterTopFront, Channel::TopFrontCenter)  \
    C(RightTopFront, Channel::TopFrontRight)    \
    C(LeftTopMiddle, Channel::TopSideLeft)      \
    C(RightTopMiddle, Channel::TopSideRight)    \
    C(LeftTopRear, Channel::TopBackLeft)        \
    C(CenterTopRear, Channel::TopBackCenter)    \
    C(RightTopRear, Channel::TopBackRight)

static Channel channel_of_bitmap_bit(u32 bit)
{
#define CHANNEL_OF_BITMAP_BIT(core_audio_channel_name, audio_channel) \
    case kAudioChannelBit_##core_audio_channel_name:                  \
        return audio_channel;

    switch (bit) {
        ENUMERATE_CHANNEL_POSITIONS(CHANNEL_OF_BITMAP_BIT)
    default:
        return Channel::Unknown;
    }
}

ErrorOr<ChannelMap> core_audio_channel_layout_to_channel_map(AudioChannelLayout const& channel_layout, u32 channel_layout_size)
{
    check_core_audio_channel_layout_size(channel_layout, channel_layout_size);

    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_Mono)
        return ChannelMap::mono();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_Stereo
        || channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_StereoHeadphones)
        return ChannelMap::stereo();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_Quadraphonic)
        return ChannelMap::quadrophonic();
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_AudioUnit_5_1)
        return ChannelMap::surround_5_1();
    // This tag puts its side surround channels before its rear surround channels.
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_AudioUnit_7_1)
        return ChannelMap(Channel::FrontLeft, Channel::FrontRight, Channel::FrontCenter, Channel::LowFrequency, Channel::SideLeft, Channel::SideRight, Channel::BackLeft, Channel::BackRight);

    Vector<Channel, ChannelMap::capacity()> channels;

#define MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION(core_audio_channel_name, audio_channel) \
    case kAudioChannelLabel_##core_audio_channel_name:                                     \
        channels.unchecked_append(audio_channel);                                          \
        break;

    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
        // The channels come in the order of their bits.
        u32 const bitmap = channel_layout.mChannelBitmap;
        for (u32 bit_index = 0; bit_index < 32; bit_index++) {
            u32 const bit = 1u << bit_index;
            if ((bitmap & bit) == 0)
                continue;
            if (channels.size() == ChannelMap::capacity())
                return Error::from_string_literal("Device channel layout had too many channels");
            channels.unchecked_append(channel_of_bitmap_bit(bit));
        }
    } else {
        auto fill_channels_from_channel_descriptions = [&](AudioChannelLayout const& channel_layout) -> ErrorOr<void> {
            VERIFY(channel_layout.mNumberChannelDescriptions > 0);
            if (channel_layout.mNumberChannelDescriptions > ChannelMap::capacity())
                return Error::from_string_literal("Channel layout had too many channels");
            auto const* channel_descriptions = &channel_layout.mChannelDescriptions[0];
            for (u32 i = 0; i < channel_layout.mNumberChannelDescriptions; i++) {
                switch (channel_descriptions[i].mChannelLabel) {
                    ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION)

                default:
                    channels.unchecked_append(Channel::Unknown);
                    break;
                }
            }
            return {};
        };

        if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
            TRY(fill_channels_from_channel_descriptions(channel_layout));
        } else {
            u32 explicit_layout_size = 0;
            if (auto status = AudioFormatGetPropertyInfo(
                    kAudioFormatProperty_ChannelLayoutForTag,
                    sizeof(AudioChannelLayoutTag),
                    &channel_layout.mChannelLayoutTag,
                    &explicit_layout_size);
                status != noErr)
                return Error::from_errno(status);
            VERIFY(explicit_layout_size >= sizeof(AudioChannelLayout));

            auto* explicit_layout = reinterpret_cast<AudioChannelLayout*>(kmalloc(explicit_layout_size));
            VERIFY(explicit_layout);
            ScopeGuard free_explicit_layout { [&] { kfree(explicit_layout); } };

            if (auto status = AudioFormatGetProperty(
                    kAudioFormatProperty_ChannelLayoutForTag,
                    sizeof(AudioChannelLayoutTag),
                    &channel_layout.mChannelLayoutTag,
                    &explicit_layout_size,
                    explicit_layout);
                status != noErr)
                return Error::from_errno(status);
            check_core_audio_channel_layout_size(*explicit_layout, explicit_layout_size);
            TRY(fill_channels_from_channel_descriptions(*explicit_layout));
        }
    }

    return ChannelMap(channels);
}

}
