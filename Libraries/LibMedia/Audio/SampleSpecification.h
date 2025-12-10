/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Audio/ChannelMap.h>

namespace Audio {

class SampleSpecification {
public:
    SampleSpecification() = default;

    SampleSpecification(u32 sample_rate, ChannelMap channel_map)
        : m_sample_rate(sample_rate)
        , m_channel_map(channel_map)
    {
    }

    bool is_valid() const
    {
        return m_sample_rate > 0 && m_channel_map.channel_count() > 0;
    }

    u32 sample_rate() const { return m_sample_rate; }
    ChannelMap const& channel_map() const { return m_channel_map; }
    u8 channel_count() const { return channel_map().channel_count(); }

    [[nodiscard]] constexpr bool operator==(SampleSpecification const& other) const
    {
        return sample_rate() == other.sample_rate() && channel_map() == other.channel_map();
    }

private:
    u32 m_sample_rate { 0 };
    ChannelMap m_channel_map;
};

}

namespace AK {

template<>
struct Formatter<Audio::SampleSpecification> : StandardFormatter {
    static ErrorOr<void> format(FormatBuilder& builder, Audio::SampleSpecification sample_specification)
    {
        return builder.builder().try_appendff("{} Hz, {}", sample_specification.sample_rate(), sample_specification.channel_map());
    }
};

}
