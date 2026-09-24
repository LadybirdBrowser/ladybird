/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/Track.h>

namespace IPC {

// The receiving process does not decode, so the track's codec stays behind.
template<>
ErrorOr<void> encode(Encoder& encoder, Media::Track const& track)
{
    TRY(encoder.encode(track.type()));
    TRY(encoder.encode(static_cast<u64>(track.identifier())));
    TRY(encoder.encode(track.kind()));
    TRY(encoder.encode(track.label()));
    TRY(encoder.encode(track.language()));
    switch (track.type()) {
    case Media::TrackType::Video: {
        auto const& video_data = track.video_data();
        TRY(encoder.encode(video_data.pixel_width));
        TRY(encoder.encode(video_data.pixel_height));
        TRY(encoder.encode(video_data.cicp));
        break;
    }
    case Media::TrackType::Audio: {
        auto const& sample_specification = track.audio_data().sample_specification;
        TRY(encoder.encode(sample_specification.sample_rate()));
        auto const& channel_map = sample_specification.channel_map();
        TRY(encoder.encode(channel_map.channel_count()));
        for (u8 i = 0; i < channel_map.channel_count(); i++)
            TRY(encoder.encode(channel_map.channel_at(i)));
        break;
    }
    case Media::TrackType::Subtitles:
    case Media::TrackType::Unknown:
        break;
    }
    return {};
}

template<>
ErrorOr<Media::Track> decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Media::TrackType>());
    if (type > Media::TrackType::Unknown)
        return Error::from_string_literal("IPC: Invalid track type");
    auto identifier = TRY(decoder.decode<u64>());
    auto kind = TRY(decoder.decode<Media::Track::Kind>());
    if (kind > Media::Track::Kind::Commentary)
        return Error::from_string_literal("IPC: Invalid track kind");
    auto label = TRY(decoder.decode<Utf16String>());
    auto language = TRY(decoder.decode<Utf16String>());

    Media::Track track { type, identifier, kind, label, language };
    switch (type) {
    case Media::TrackType::Video: {
        Media::Track::VideoData video_data;
        video_data.pixel_width = TRY(decoder.decode<u64>());
        video_data.pixel_height = TRY(decoder.decode<u64>());
        video_data.cicp = TRY(decoder.decode<Media::CodingIndependentCodePoints>());
        track.set_video_data(video_data);
        break;
    }
    case Media::TrackType::Audio: {
        auto sample_rate = TRY(decoder.decode<u32>());
        auto channel_count = TRY(decoder.decode<u8>());
        if (channel_count > Audio::ChannelMap::capacity())
            return Error::from_string_literal("IPC: Too many audio channels");
        Vector<Audio::Channel, Audio::ChannelMap::capacity()> channels;
        for (u8 i = 0; i < channel_count; i++) {
            auto channel = TRY(decoder.decode<Audio::Channel>());
            if (channel >= Audio::Channel::Count)
                return Error::from_string_literal("IPC: Invalid audio channel");
            channels.append(channel);
        }
        track.set_audio_data({ Audio::SampleSpecification { sample_rate, Audio::ChannelMap { channels } } });
        break;
    }
    case Media::TrackType::Subtitles:
    case Media::TrackType::Unknown:
        break;
    }
    return track;
}

}
