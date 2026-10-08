/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/Audio/SampleSpecification.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Audio::SampleSpecification const& specification)
{
    TRY(encoder.encode(specification.sample_rate()));
    auto const& channel_map = specification.channel_map();
    TRY(encoder.encode(channel_map.channel_count()));
    for (u8 i = 0; i < channel_map.channel_count(); i++)
        TRY(encoder.encode(to_underlying(channel_map.channel_at(i))));
    return {};
}

template<>
ErrorOr<Audio::SampleSpecification> decode(Decoder& decoder)
{
    auto sample_rate = TRY(decoder.decode<u32>());
    auto channel_count = TRY(decoder.decode<u8>());
    if (channel_count > Audio::ChannelMap::capacity())
        return Error::from_string_literal("Sample specification has too many channels");
    Vector<Audio::Channel, to_underlying(Audio::Channel::Count)> channels;
    for (u8 i = 0; i < channel_count; i++) {
        auto channel = TRY(decoder.decode<u8>());
        if (channel >= to_underlying(Audio::Channel::Count))
            return Error::from_string_literal("Sample specification has an invalid channel");
        channels.append(static_cast<Audio::Channel>(channel));
    }
    return Audio::SampleSpecification { sample_rate, Audio::ChannelMap { channels } };
}

}
