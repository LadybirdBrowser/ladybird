/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/VideoDecoder.h>

namespace Media::FFmpeg {

// The Vulkan video decoder as compiled against one libavcodec major version's headers, decoding with one library of
// that version.
class VulkanVideoDecoderBackend {
public:
    virtual ~VulkanVideoDecoderBackend() = default;

    virtual Optional<DecoderCapabilities> capabilities(ParsedCodec const&) const = 0;
    virtual DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data) const = 0;
};

struct FFmpegFunctions;

// The decoder as compiled against the headers of the FFmpeg that Ladybird is built with. A system FFmpeg has one only
// if it is of the same version.
VulkanVideoDecoderBackend const& vulkan_video_decoder_backend_for_bundled_ffmpeg();
VulkanVideoDecoderBackend const* vulkan_video_decoder_backend_for_system_ffmpeg(FFmpegFunctions const&);

// The decoder as compiled against the headers of another libavcodec major version, which is null unless the library is
// of that version. It is specialized for each major whose headers this build has.
template<unsigned LibAVCodecMajor>
VulkanVideoDecoderBackend const* vulkan_video_decoder_backend_for_library(void* library_handle);

}
