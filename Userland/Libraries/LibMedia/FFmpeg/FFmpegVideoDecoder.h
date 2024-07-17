/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>
#include <LibMedia/VideoDecoder.h>

#include "FFmpegForward.h"

namespace Media::FFmpeg {

class FFmpegVideoDecoder final : public VideoDecoder {
public:
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame);
    ~FFmpegVideoDecoder();

    DecoderErrorOr<void> receive_sample(AK::Duration timestamp, ReadonlyBytes sample) override;
    DecoderErrorOr<NonnullOwnPtr<VideoFrame>> get_decoded_frame() override;

    void flush() override;

private:
    DecoderErrorOr<void> decode_single_sample(AK::Duration timestamp, u8* data, int size);

    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
};

}
