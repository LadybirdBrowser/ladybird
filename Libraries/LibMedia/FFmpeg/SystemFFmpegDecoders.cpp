/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/FFmpeg/SystemFFmpeg.h>
#include <LibMedia/FFmpeg/SystemFFmpegDecoders.h>

namespace Media::FFmpeg {

Optional<DecoderCapabilities> system_ffmpeg_video_decoder_capabilities(ParsedCodec const& codec)
{
    auto const* library = SystemFFmpeg::the();
    if (library == nullptr)
        return {};
    return FFmpegVideoDecoder::capabilities(library->functions(), codec);
}

Optional<DecoderCapabilities> system_ffmpeg_audio_decoder_capabilities(ParsedCodec const& codec)
{
    auto const* library = SystemFFmpeg::the();
    if (library == nullptr)
        return {};
    return FFmpegAudioDecoder::capabilities(library->functions(), codec);
}

DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_system_ffmpeg_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto const* library = SystemFFmpeg::the();
    if (library == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No system FFmpeg is loaded"sv);
    return NonnullOwnPtr<VideoDecoder> { TRY(FFmpegVideoDecoder::try_create(library->functions(), codec_id, codec_initialization_data)) };
}

DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_system_ffmpeg_audio_decoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    auto const* library = SystemFFmpeg::the();
    if (library == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No system FFmpeg is loaded"sv);
    return NonnullOwnPtr<AudioDecoder> { TRY(FFmpegAudioDecoder::try_create(library->functions(), codec_id, sample_specification, codec_initialization_data)) };
}

}
