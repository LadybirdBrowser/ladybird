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

DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audio_decoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    if (track_type_from_codec_id(codec_id) != TrackType::Audio)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "{} is not an audio codec", codec_id);

    for (auto const& registration : audio_decoders_in_priority_order) {
        auto decoder = registration.create(codec_id, sample_specification, codec_initialization_data);
        if (!decoder.is_error())
            return decoder.release_value();

        auto error = decoder.release_error();
        if (error.category() != DecoderErrorCategory::NotImplemented)
            return error;
    }

    return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find an audio decoder for codec {}", codec_id);
}

DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    if (track_type_from_codec_id(codec_id) != TrackType::Video)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "{} is not a video codec", codec_id);

    for (auto const& registration : video_decoders_in_priority_order) {
        auto decoder = registration.create(codec_id, codec_initialization_data);
        if (!decoder.is_error())
            return decoder.release_value();

        auto error = decoder.release_error();
        if (error.category() != DecoderErrorCategory::NotImplemented)
            return error;
    }

    return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find a video decoder for codec {}", codec_id);
}

}
