/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/RemoteVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>
#ifdef AK_OS_MACOS
#    include <LibMedia/AudioToolbox/AudioToolboxAudioDecoder.h>
#endif
#ifdef AK_OS_LINUX
#    include <LibMedia/FFmpeg/SystemFFmpegDecoders.h>
#endif
#ifdef AK_OS_WINDOWS
#    include <LibMedia/WindowsMediaFoundation/WindowsMediaFoundationAudioDecoder.h>
#endif

namespace Media {

static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_remote_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(RemoteVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}

ReadonlySpan<VideoDecoderRegistration> remote_video_decoders()
{
    static Array const decoders {
        VideoDecoderRegistration { RemoteVideoDecoder::capabilities, create_remote_video_decoder },
    };
    return decoders;
}

static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_ffmpeg_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}

ReadonlySpan<VideoDecoderRegistration> ffmpeg_video_decoders()
{
    static Array const decoders {
        VideoDecoderRegistration { FFmpeg::FFmpegVideoDecoder::capabilities, create_ffmpeg_video_decoder },
#ifdef AK_OS_LINUX
        VideoDecoderRegistration { FFmpeg::system_ffmpeg_video_decoder_capabilities, FFmpeg::create_system_ffmpeg_video_decoder },
#endif
    };
    return decoders;
}

static DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_ffmpeg_audio_decoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<AudioDecoder> { TRY(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, sample_specification, codec_initialization_data)) };
}

#ifdef AK_OS_MACOS
static DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audiotoolbox_audio_decoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<AudioDecoder> { TRY(AudioToolbox::AudioToolboxAudioDecoder::try_create(codec_id, sample_specification, codec_initialization_data)) };
}
#endif

#ifdef AK_OS_WINDOWS
static DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_windows_media_foundation_audio_decoder(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<AudioDecoder> { TRY(WindowsMediaFoundation::WindowsMediaFoundationAudioDecoder::try_create(codec_id, sample_specification, codec_initialization_data)) };
}
#endif

ReadonlySpan<AudioDecoderRegistration> audio_decoders()
{
    static constexpr Array decoders {
#ifdef AK_OS_MACOS
        AudioDecoderRegistration { AudioToolbox::AudioToolboxAudioDecoder::capabilities, create_audiotoolbox_audio_decoder },
#endif
#ifdef AK_OS_WINDOWS
        AudioDecoderRegistration { WindowsMediaFoundation::WindowsMediaFoundationAudioDecoder::capabilities, create_windows_media_foundation_audio_decoder },
#endif
        AudioDecoderRegistration { FFmpeg::FFmpegAudioDecoder::capabilities, create_ffmpeg_audio_decoder },
#ifdef AK_OS_LINUX
        AudioDecoderRegistration { FFmpeg::system_ffmpeg_audio_decoder_capabilities, FFmpeg::create_system_ffmpeg_audio_decoder },
#endif
    };
    return decoders;
}

}
