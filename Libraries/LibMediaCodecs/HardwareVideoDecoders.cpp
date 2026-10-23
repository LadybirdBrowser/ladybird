/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMediaCodecs/HardwareVideoDecoders.h>
#ifdef AK_OS_MACOS
#    include <LibMediaCodecs/VideoToolbox/VideoToolboxVideoDecoder.h>
#endif
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoder.h>
#endif
#ifdef AK_OS_WINDOWS
#    include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationVideoDecoder.h>
#endif

namespace Media {

#ifdef AK_OS_MACOS
static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_videotoolbox_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(VideoToolbox::VideoToolboxVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_vulkan_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(FFmpeg::FFmpegVulkanVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}
#endif

#ifdef AK_OS_WINDOWS
static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_windows_media_foundation_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(WindowsMediaFoundation::WindowsMediaFoundationVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}
#endif

ReadonlySpan<VideoDecoderRegistration> hardware_video_decoders()
{
#if defined(AK_OS_MACOS)
    static constexpr Array decoders {
        VideoDecoderRegistration { VideoToolbox::VideoToolboxVideoDecoder::capabilities, create_videotoolbox_video_decoder },
    };
    return decoders;
#elif defined(AK_OS_WINDOWS)
    // These are the system's software decoders until the transforms are given a D3D11 device to decode on.
    static constexpr Array decoders {
        VideoDecoderRegistration { WindowsMediaFoundation::WindowsMediaFoundationVideoDecoder::capabilities, create_windows_media_foundation_video_decoder },
    };
    return decoders;
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    // Without a Vulkan device in the process, this answers no capabilities and creates nothing.
    static constexpr Array decoders {
        VideoDecoderRegistration { FFmpeg::FFmpegVulkanVideoDecoder::capabilities, create_vulkan_video_decoder },
    };
    return decoders;
#else
    return {};
#endif
}

}
