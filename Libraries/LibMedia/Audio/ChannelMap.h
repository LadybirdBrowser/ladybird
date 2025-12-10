/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/TypedTransfer.h>
#include <AK/Types.h>

namespace Audio {

enum class Channel : u8 {
    Unknown,
    FrontLeft,
    FrontRight,
    FrontCenter,
    LowFrequency,
    BackLeft,
    BackRight,
    FrontLeftOfCenter,
    FrontRightOfCenter,
    BackCenter,
    SideLeft,
    SideRight,
    TopCenter,
    TopFrontLeft,
    TopFrontCenter,
    TopFrontRight,
    TopBackLeft,
    TopBackCenter,
    TopBackRight,
    Count,
};

class ChannelMap {
public:
    static constexpr ChannelMap mono()
    {
        return ChannelMap(Channel::FrontCenter);
    }

    static constexpr ChannelMap stereo()
    {
        return ChannelMap(Channel::FrontLeft, Channel::FrontRight);
    }

    static constexpr ChannelMap quadrophonic()
    {
        return ChannelMap(
            Channel::FrontLeft,
            Channel::FrontRight,
            Channel::BackLeft,
            Channel::BackRight);
    }

    static constexpr ChannelMap surround_5_1()
    {
        return ChannelMap(
            Channel::FrontLeft,
            Channel::FrontRight,
            Channel::FrontCenter,
            Channel::LowFrequency,
            Channel::BackLeft,
            Channel::BackRight);
    }

    static constexpr ChannelMap surround_7_1()
    {
        return ChannelMap(
            Channel::FrontLeft,
            Channel::FrontRight,
            Channel::FrontCenter,
            Channel::LowFrequency,
            Channel::BackLeft,
            Channel::BackRight,
            Channel::SideLeft,
            Channel::SideRight);
    }

    template<typename... Channels>
    requires(IsSame<Channels, Channel> && ...)
    constexpr ChannelMap(Channels... channels)
        : m_channel_count(sizeof...(channels) / sizeof(Channel))
        , m_channels(channels...)
    {
    }

    template<size_t VecCapacity>
    ChannelMap(Vector<Channel, VecCapacity> const& channels)
        : ChannelMap(channels.span())
    {
    }

    ChannelMap(ReadonlySpan<Channel> channels)
        : m_channel_count(channels.size())
    {
        VERIFY(channels.size() <= capacity());
        AK::TypedTransfer<Channel>::copy(m_channels, channels.data(), channels.size());
    }

    u8 channel_count() const { return m_channel_count; }
    Channel channel_at(u8 index) const
    {
        VERIFY(index < channel_count());
        return m_channels[index];
    }

    static constexpr size_t capacity() { return sizeof(m_channels) / sizeof(*m_channels); }

    [[nodiscard]] constexpr bool operator==(ChannelMap const& other) const
    {
        if (channel_count() != other.channel_count())
            return false;
        for (u8 i = 0; i < channel_count(); i++) {
            if (channel_at(i) != other.channel_at(i))
                return false;
        }
        return true;
    }

private:
    u8 m_channel_count { 0 };
    Channel m_channels[to_underlying(Channel::Count)];
};

constexpr StringView audio_channel_to_string(Channel channel)
{
    switch (channel) {
    case Audio::Channel::Unknown:
        return "None"sv;
    case Audio::Channel::FrontLeft:
        return "FrontLeft"sv;
    case Audio::Channel::FrontRight:
        return "FrontRight"sv;
    case Audio::Channel::FrontCenter:
        return "FrontCenter"sv;
    case Audio::Channel::LowFrequency:
        return "LowFrequency"sv;
    case Audio::Channel::BackLeft:
        return "BackLeft"sv;
    case Audio::Channel::BackRight:
        return "BackRight"sv;
    case Audio::Channel::FrontLeftOfCenter:
        return "FrontLeftOfCenter"sv;
    case Audio::Channel::FrontRightOfCenter:
        return "FrontRightOfCenter"sv;
    case Audio::Channel::BackCenter:
        return "BackCenter"sv;
    case Audio::Channel::SideLeft:
        return "SideLeft"sv;
    case Audio::Channel::SideRight:
        return "SideRight"sv;
    case Audio::Channel::TopCenter:
        return "TopCenter"sv;
    case Audio::Channel::TopFrontLeft:
        return "TopFrontLeft"sv;
    case Audio::Channel::TopFrontCenter:
        return "TopFrontCenter"sv;
    case Audio::Channel::TopFrontRight:
        return "TopFrontRight"sv;
    case Audio::Channel::TopBackLeft:
        return "TopBackLeft"sv;
    case Audio::Channel::TopBackCenter:
        return "TopBackCenter"sv;
    case Audio::Channel::TopBackRight:
        return "TopBackRight"sv;
    case Audio::Channel::Count:
        break;
    }
    VERIFY_NOT_REACHED();
}

}

namespace AK {

template<>
struct Formatter<Audio::Channel> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Audio::Channel channel)
    {
        return Formatter<StringView>::format(builder, Audio::audio_channel_to_string(channel));
    }
};

template<>
struct Formatter<Audio::ChannelMap> : StandardFormatter {
    static ErrorOr<void> format(FormatBuilder& builder, Audio::ChannelMap channel_map)
    {
        TRY(builder.builder().try_append("[ "sv));
        auto count = channel_map.channel_count();
        for (u8 i = 0; i < count; i++)
            TRY(builder.builder().try_appendff("{}, ", channel_map.channel_at(i)));
        if (count > 0)
            builder.builder().trim(2);
        TRY(builder.builder().try_append(" ]"sv));
        return {};
    }
};

}
