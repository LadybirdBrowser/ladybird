/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibMedia/Export.h>
#include <LibMedia/I420Picture.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

// Encodes each picture before the next arrives, using only earlier frames as references. Produces key frames on request.
class MEDIA_API FFmpegVideoEncoder final {
    AK_ALLOC_WITH_KMALLOC;

public:
    struct Frame {
        ByteBuffer data;
        bool is_key_frame { false };
    };

    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoEncoder>> try_create(CodecID, u32 width, u32 height, int bitrate);
    FFmpegVideoEncoder(FFmpegFunctions const&, AVCodecContext*, AVPacket*, AVFrame*);
    ~FFmpegVideoEncoder();

    u32 width() const;
    u32 height() const;

    // Timestamps must increase. Returns no frame when the encoder drops a picture to meet its bitrate.
    DecoderErrorOr<Optional<Frame>> encode(I420Picture const&, AK::Duration timestamp, bool as_key_frame);

private:
    FFmpegFunctions const& m_functions;
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
};

}
