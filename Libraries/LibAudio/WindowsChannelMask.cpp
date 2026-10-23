/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibAudio/WindowsChannelMask.h>

#include <AK/Windows.h>
#include <mmreg.h>

namespace Audio {

// This needs to be kept up to date with KSAUDIO_CHANNEL_LAYOUT in ksmedia.h
#define ENUMERATE_CHANNEL_POSITIONS(C)                            \
    C(SPEAKER_FRONT_LEFT, Channel::FrontLeft)                     \
    C(SPEAKER_FRONT_RIGHT, Channel::FrontRight)                   \
    C(SPEAKER_FRONT_CENTER, Channel::FrontCenter)                 \
    C(SPEAKER_LOW_FREQUENCY, Channel::LowFrequency)               \
    C(SPEAKER_BACK_LEFT, Channel::BackLeft)                       \
    C(SPEAKER_BACK_RIGHT, Channel::BackRight)                     \
    C(SPEAKER_FRONT_LEFT_OF_CENTER, Channel::FrontLeftOfCenter)   \
    C(SPEAKER_FRONT_RIGHT_OF_CENTER, Channel::FrontRightOfCenter) \
    C(SPEAKER_BACK_CENTER, Channel::BackCenter)                   \
    C(SPEAKER_SIDE_LEFT, Channel::SideLeft)                       \
    C(SPEAKER_SIDE_RIGHT, Channel::SideRight)                     \
    C(SPEAKER_TOP_CENTER, Channel::TopCenter)                     \
    C(SPEAKER_TOP_FRONT_LEFT, Channel::TopFrontLeft)              \
    C(SPEAKER_TOP_FRONT_CENTER, Channel::TopFrontCenter)          \
    C(SPEAKER_TOP_FRONT_RIGHT, Channel::TopFrontRight)            \
    C(SPEAKER_TOP_BACK_LEFT, Channel::TopBackLeft)                \
    C(SPEAKER_TOP_BACK_CENTER, Channel::TopBackCenter)            \
    C(SPEAKER_TOP_BACK_RIGHT, Channel::TopBackRight)

ErrorOr<ChannelMap> windows_channel_mask_to_channel_map(u32 channel_mask)
{
    Vector<Channel, ChannelMap::capacity()> channels;

#define MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG(ksmedia_channel_name, audio_channel)        \
    if ((channel_mask & ksmedia_channel_name) != 0) {                                  \
        if (channels.size() == ChannelMap::capacity()) [[unlikely]]                    \
            return Error::from_string_literal("Channel layout had too many channels"); \
        channels.unchecked_append(audio_channel);                                      \
    }

    ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG);

    if ((channel_mask & SPEAKER_RESERVED) != 0) [[unlikely]]
        return Error::from_string_literal("Unsupported new KSMEDIA version");

    return ChannelMap { channels };
}

}
