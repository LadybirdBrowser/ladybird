/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>

namespace Media {

using CapabilitiesFunction = Optional<DecoderCapabilities> (*)(ParsedCodec const&);
using AudioDecoderFactory = DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> (*)(CodecID, Audio::SampleSpecification const&, ReadonlyBytes);
using VideoDecoderFactory = DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> (*)(CodecID, ReadonlyBytes);

struct AudioDecoderRegistration {
    CapabilitiesFunction capabilities;
    AudioDecoderFactory create;
};

struct VideoDecoderRegistration {
    CapabilitiesFunction capabilities;
    VideoDecoderFactory create;
};

static DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_ffmpeg_audio_decoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<AudioDecoder> { TRY(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, sample_specification, codec_initialization_data)) };
}

static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_ffmpeg_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}

static constexpr Array audio_decoders_in_priority_order {
    AudioDecoderRegistration { FFmpeg::FFmpegAudioDecoder::capabilities, create_ffmpeg_audio_decoder },
};

static constexpr Array video_decoders_in_priority_order {
    VideoDecoderRegistration { FFmpeg::FFmpegVideoDecoder::capabilities, create_ffmpeg_video_decoder },
};

Optional<DecoderCapabilities> decoder_capabilities(ParsedCodec const& codec)
{
    auto find_capabilities = [&](auto const& registrations) -> Optional<DecoderCapabilities> {
        for (auto const& registration : registrations) {
            auto capabilities = registration.capabilities(codec);
            if (capabilities.has_value())
                return capabilities;
        }
        return {};
    };

    switch (track_type_from_codec_id(codec.codec_id())) {
    case TrackType::Audio:
        return find_capabilities(audio_decoders_in_priority_order);
    case TrackType::Video:
        return find_capabilities(video_decoders_in_priority_order);
    case TrackType::Subtitles:
    case TrackType::Unknown:
        return {};
    }
    VERIFY_NOT_REACHED();
}

AudioDecoderSelection select_audio_decoder(ParsedCodec const& codec, AudioDecoderSelection after)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Audio)
        return {};

    auto registration_count = static_cast<i32>(audio_decoders_in_priority_order.size());
    for (auto index = after.registration_index() + 1; index < registration_count; index++) {
        if (audio_decoders_in_priority_order[index].capabilities(codec).has_value())
            return AudioDecoderSelection { index };
    }
    return {};
}

DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audio_decoder(AudioDecoderSelection selection, CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    VERIFY(selection.has_value());
    VERIFY(selection.registration_index() < static_cast<i32>(audio_decoders_in_priority_order.size()));
    return audio_decoders_in_priority_order[selection.registration_index()].create(codec_id, sample_specification, codec_initialization_data);
}

VideoDecoderSelection select_video_decoder(ParsedCodec const& codec, VideoDecoderSelection after)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video)
        return {};

    auto registration_count = static_cast<i32>(video_decoders_in_priority_order.size());
    for (auto index = after.registration_index() + 1; index < registration_count; index++) {
        if (video_decoders_in_priority_order[index].capabilities(codec).has_value())
            return VideoDecoderSelection { index };
    }
    return {};
}

DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_video_decoder(VideoDecoderSelection selection, CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    VERIFY(selection.has_value());
    VERIFY(selection.registration_index() < static_cast<i32>(video_decoders_in_priority_order.size()));
    return video_decoders_in_priority_order[selection.registration_index()].create(codec_id, codec_initialization_data);
}

}
