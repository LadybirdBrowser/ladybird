/*
 * Copyright (c) 2024-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FFmpegHelpers.h"

namespace Media::FFmpeg {

#define ENUMERATE_CHANNEL_POSITIONS(C)                                              \
    C(Audio::Channel::FrontLeft, AVChannel::AV_CHAN_FRONT_LEFT)                     \
    C(Audio::Channel::FrontRight, AVChannel::AV_CHAN_FRONT_RIGHT)                   \
    C(Audio::Channel::FrontCenter, AVChannel::AV_CHAN_FRONT_CENTER)                 \
    C(Audio::Channel::LowFrequency, AVChannel::AV_CHAN_LOW_FREQUENCY)               \
    C(Audio::Channel::BackLeft, AVChannel::AV_CHAN_BACK_LEFT)                       \
    C(Audio::Channel::BackRight, AVChannel::AV_CHAN_BACK_RIGHT)                     \
    C(Audio::Channel::FrontLeftOfCenter, AVChannel::AV_CHAN_FRONT_LEFT_OF_CENTER)   \
    C(Audio::Channel::FrontRightOfCenter, AVChannel::AV_CHAN_FRONT_RIGHT_OF_CENTER) \
    C(Audio::Channel::BackCenter, AVChannel::AV_CHAN_BACK_CENTER)                   \
    C(Audio::Channel::SideLeft, AVChannel::AV_CHAN_SIDE_LEFT)                       \
    C(Audio::Channel::SideRight, AVChannel::AV_CHAN_SIDE_RIGHT)                     \
    C(Audio::Channel::TopCenter, AVChannel::AV_CHAN_TOP_CENTER)                     \
    C(Audio::Channel::TopFrontLeft, AVChannel::AV_CHAN_TOP_FRONT_LEFT)              \
    C(Audio::Channel::TopFrontCenter, AVChannel::AV_CHAN_TOP_FRONT_CENTER)          \
    C(Audio::Channel::TopFrontRight, AVChannel::AV_CHAN_TOP_FRONT_RIGHT)            \
    C(Audio::Channel::TopBackLeft, AVChannel::AV_CHAN_TOP_BACK_LEFT)                \
    C(Audio::Channel::TopBackCenter, AVChannel::AV_CHAN_TOP_BACK_CENTER)            \
    C(Audio::Channel::TopBackRight, AVChannel::AV_CHAN_TOP_BACK_RIGHT)

ErrorOr<Audio::ChannelMap> av_channel_layout_to_channel_map(AVChannelLayout const& layout)
{
    if (layout.nb_channels <= 0)
        return Error::from_string_literal("FFmpeg channel layout had no channels");
    if (static_cast<size_t>(layout.nb_channels) > Audio::ChannelMap::capacity())
        return Error::from_string_literal("FFmpeg channel layout had too many channels");
    Vector<Audio::Channel, Audio::ChannelMap::capacity()> channels;
    channels.resize(layout.nb_channels);

    if (layout.order == AVChannelOrder::AV_CHANNEL_ORDER_UNSPEC) {
        if (layout.nb_channels == 1)
            return Audio::ChannelMap::mono();
        if (layout.nb_channels == 2)
            return Audio::ChannelMap::stereo();
        if (layout.nb_channels == 4)
            return Audio::ChannelMap::quadrophonic();
        if (layout.nb_channels == 6)
            return Audio::ChannelMap::surround_5_1();
        if (layout.nb_channels == 8)
            return Audio::ChannelMap::surround_7_1();

        for (int i = 0; i < layout.nb_channels; ++i)
            channels[i] = Audio::Channel::Unknown;
        return Audio::ChannelMap(channels);
    }

#define AV_CHANNEL_TO_AUDIO_CHANNEL(audio_channel, av_channel) \
    case av_channel:                                           \
        return audio_channel;

    for (int i = 0; i < layout.nb_channels; i++) {
        auto channel = [&] {
            switch (av_channel_layout_channel_from_index(&layout, i)) {
                ENUMERATE_CHANNEL_POSITIONS(AV_CHANNEL_TO_AUDIO_CHANNEL)
            default:
                return Audio::Channel::Unknown;
            }
        }();
        channels[i] = channel;
    }

    return Audio::ChannelMap(channels);
}

static AVChannel audio_channel_to_av_channel(Audio::Channel channel)
{
#define AUDIO_CHANNEL_TO_AV_CHANNEL(audio_channel, av_channel) \
    case audio_channel:                                        \
        return av_channel;

    switch (channel) {
        ENUMERATE_CHANNEL_POSITIONS(AUDIO_CHANNEL_TO_AV_CHANNEL)
    default:
        return AVChannel::AV_CHAN_UNKNOWN;
    }
}

static ErrorOr<AVChannelLayout> channel_map_to_custom_av_channel_layout(Audio::ChannelMap const& channel_map)
{
    AVChannelLayout layout;
    auto init_result = av_channel_layout_custom_init(&layout, channel_map.channel_count());
    if (init_result == AVERROR(EINVAL))
        return Error::from_string_literal("Attempted to create an FFmpeg channel layout with an invalid channel count");
    if (init_result == AVERROR(ENOMEM))
        return Error::from_string_literal("Failed to allocate an FFmpeg channel layout");
    VERIFY(layout.nb_channels == channel_map.channel_count());

    for (u8 i = 0; i < channel_map.channel_count(); i++)
        layout.u.map[i].id = audio_channel_to_av_channel(channel_map.channel_at(i));

    return layout;
}

ErrorOr<AVChannelLayout> channel_map_to_av_channel_layout(Audio::ChannelMap const& channel_map)
{
    if (channel_map.channel_count() > NumericLimits<decltype(AVChannelLayout::u.mask)>::digits())
        return channel_map_to_custom_av_channel_layout(channel_map);

    AVChannelLayout layout;
    layout.nb_channels = channel_map.channel_count();
    layout.order = AVChannelOrder::AV_CHANNEL_ORDER_NATIVE;
    layout.u.map = nullptr;
    layout.u.mask = 0;
    layout.opaque = nullptr;

    auto last_channel = AVChannel::AV_CHAN_NONE;
    for (u8 i = 0; i < channel_map.channel_count(); i++) {
        auto channel = audio_channel_to_av_channel(channel_map.channel_at(i));
        // Native order follows the order of the declarations in AVChannel, which are sequential.
        // If we find that one of the channels in our input mapping doesn't match this requirement,
        // fall back to a custom order.
        if (channel <= last_channel)
            return channel_map_to_custom_av_channel_layout(channel_map);
        layout.u.mask |= 1ULL << channel;
    }

    return layout;
}

}
