/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/NonnullOwnPtr.h>
#    include <AK/Optional.h>
#    include <AK/Span.h>
#    include <LibMediaCodecs/CodecID.h>
#    include <LibMediaCodecs/CodecParameters.h>
#    include <LibMediaCodecs/DecoderCapabilities.h>
#    include <LibMediaCodecs/DecoderError.h>
#    include <LibMediaCodecs/Export.h>
#    include <LibMediaCodecs/VideoDecoder.h>

namespace Media::FFmpeg {

// Decodes through FFmpeg's Vulkan hardware acceleration: VP9 and AV1 with the bundled FFmpeg, and H.264 and HEVC,
// which we ship no decoders for, with the system's.
class MEDIACODECS_API FFmpegVulkanVideoDecoder {
public:
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);

    // Whether this build has a decoder compiled for the version of the system FFmpeg that the process loaded. Each codec
    // still depends on the library and the GPU.
    static bool has_vulkan_decoder_for_system_ffmpeg();
};

}

#endif
