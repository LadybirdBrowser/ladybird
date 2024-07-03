/*
 * Copyright (c) 2024, Alex Studer <alex@studer.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibMedia/VideoFrame.h>

#include "FFmpegForward.h"
#include "FFmpegVideoDecoder.h"

namespace Media::FFmpeg {

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    (void)codec_id;
    (void)codec_initialization_data;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

FFmpegVideoDecoder::FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame)
    : m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
{
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
}

DecoderErrorOr<void> FFmpegVideoDecoder::receive_sample(Duration timestamp, ReadonlyBytes sample)
{
    (void)timestamp;
    (void)sample;
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> FFmpegVideoDecoder::get_decoded_frame()
{
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg not available on this platform");
}

void FFmpegVideoDecoder::flush()
{
}

}
