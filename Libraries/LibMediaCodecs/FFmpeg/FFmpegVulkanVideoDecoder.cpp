/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoderBackend.h>
#include <LibMediaCodecs/SystemFFmpegHeaderMajors.h>
#include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>
#include <LibPlatformFeatures/ProcessFeatures.h>

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
#    include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>
#endif

namespace Media::FFmpeg {

#define DECLARE_BACKEND_FOR_MAJOR(major) \
    template<>                           \
    VulkanVideoDecoderBackend const* vulkan_video_decoder_backend_for_library<major>(void* library_handle);
ENUMERATE_SYSTEM_FFMPEG_HEADER_MAJORS(DECLARE_BACKEND_FOR_MAJOR)
#undef DECLARE_BACKEND_FOR_MAJOR

static VulkanVideoDecoderBackend const* backend_for_system_ffmpeg()
{
#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
    auto const* library = SystemFFmpeg::the();
    if (library == nullptr)
        return nullptr;
    if (auto const* backend = vulkan_video_decoder_backend_for_system_ffmpeg(library->functions()))
        return backend;
    switch (library->major()) {
        // The decoder reaches into FFmpeg's structs, so it is compiled against the headers of each libavcodec major
        // version that this build has, other than the one Ladybird is built with.
#    define BACKEND_FOR_MAJOR(major) \
    case major:                      \
        return vulkan_video_decoder_backend_for_library<major>(library->library_handle());
        ENUMERATE_SYSTEM_FFMPEG_HEADER_MAJORS(BACKEND_FOR_MAJOR)
#    undef BACKEND_FOR_MAJOR
    default:
        return nullptr;
    }
#else
    return nullptr;
#endif
}

static VulkanVideoDecoderBackend const* backend_for(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::VP9:
    case CodecID::AV1:
        return &vulkan_video_decoder_backend_for_bundled_ffmpeg();
    case CodecID::H264:
    case CodecID::H265:
        return backend_for_system_ffmpeg();
    default:
        return nullptr;
    }
}

// Each codec's feature depends on the feature of the FFmpeg that decodes it.
static bool may_decode(CodecID codec_id)
{
    auto feature = [&] -> Optional<PlatformFeatures::Feature> {
        switch (codec_id) {
        case CodecID::VP9:
            return PlatformFeatures::Feature::VulkanVideoBundledFFmpegVP9;
        case CodecID::AV1:
            return PlatformFeatures::Feature::VulkanVideoBundledFFmpegAV1;
        case CodecID::H264:
            return PlatformFeatures::Feature::VulkanVideoSystemFFmpegH264;
        case CodecID::H265:
            return PlatformFeatures::Feature::VulkanVideoSystemFFmpegH265;
        default:
            return {};
        }
    }();
    return feature.has_value() && PlatformFeatures::ProcessFeatures::the().is_enabled(*feature);
}

Optional<DecoderCapabilities> FFmpegVulkanVideoDecoder::capabilities(ParsedCodec const& codec)
{
    // A process with no device has not necessarily set its features up either.
    if (VulkanVideoDevice::the() == nullptr)
        return {};
    auto codec_id = codec.codec_id();
    if (!may_decode(codec_id))
        return {};
    auto const* backend = backend_for(codec_id);
    if (backend == nullptr)
        return {};
    return backend->capabilities(codec);
}

DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> FFmpegVulkanVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    if (VulkanVideoDevice::the() == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No Vulkan device can decode video in this process"sv);
    auto const* backend = may_decode(codec_id) ? backend_for(codec_id) : nullptr;
    if (backend == nullptr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "No FFmpeg can decode codec {} through Vulkan", codec_id);
    return backend->try_create(codec_id, codec_initialization_data);
}

bool FFmpegVulkanVideoDecoder::has_vulkan_decoder_for_system_ffmpeg()
{
    return backend_for_system_ffmpeg() != nullptr;
}

}
