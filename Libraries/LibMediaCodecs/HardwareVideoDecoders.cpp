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

namespace Media {

#ifdef AK_OS_MACOS
static DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_videotoolbox_video_decoder(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<VideoDecoder> { TRY(VideoToolbox::VideoToolboxVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}
#endif

ReadonlySpan<VideoDecoderRegistration> hardware_video_decoders()
{
#ifdef AK_OS_MACOS
    static constexpr Array decoders {
        VideoDecoderRegistration { VideoToolbox::VideoToolboxVideoDecoder::capabilities, create_videotoolbox_video_decoder },
    };
    return decoders;
#else
    return {};
#endif
}

}
