/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibMedia/Audio/CoreAudioChannelLayout.h>

#include <AudioToolbox/AudioFormat.h>

namespace Audio {

static void check_core_audio_channel_layout_size(AudioChannelLayout const& layout, u32 size)
{
    auto minimum_layout_size = Checked(layout.mNumberChannelDescriptions);
    minimum_layout_size *= sizeof(layout.mChannelDescriptions[0]);
    minimum_layout_size += offsetof(AudioChannelLayout, mChannelDescriptions);
    VERIFY(size >= minimum_layout_size.value());
}

// This must be kept in the order defined by AudioChannelBitmap.
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
    C(RightTopFront, Channel::TopFrontRight)

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
    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_AudioUnit_7_1)
        return ChannelMap::surround_7_1();

    Vector<Channel, ChannelMap::capacity()> channels;

#define MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG(core_audio_channel_name, audio_channel)            \
    if ((channel_layout.mChannelBitmap & kAudioChannelBit_##core_audio_channel_name) != 0) {  \
        if (channels.size() == ChannelMap::capacity())                                        \
            return Error::from_string_literal("Device channel layout had too many channels"); \
        channels.unchecked_append(audio_channel);                                             \
    }

#define MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION(core_audio_channel_name, audio_channel) \
    case kAudioChannelLabel_##core_audio_channel_name:                                     \
        channels.unchecked_append(audio_channel);                                          \
        break;

    if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
        ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG);
    } else {
        auto fill_channels_from_channel_descriptions = [&](AudioChannelLayout const& channel_layout) {
            VERIFY(channel_layout.mNumberChannelDescriptions > 0);
            auto const* channel_descriptions = &channel_layout.mChannelDescriptions[0];
            for (u32 i = 0; i < channel_layout.mNumberChannelDescriptions; i++) {
                switch (channel_descriptions[i].mChannelLabel) {
                    ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION)

                default:
                    channels.unchecked_append(Channel::Unknown);
                    break;
                }
            }
        };

        if (channel_layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
            fill_channels_from_channel_descriptions(channel_layout);
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
            fill_channels_from_channel_descriptions(*explicit_layout);
        }
    }

    return ChannelMap(channels);
}

}
