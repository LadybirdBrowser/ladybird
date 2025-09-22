/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoDecoder.h>

#include "FFmpegForward.h"

namespace Media::FFmpeg {

class MEDIA_API FFmpegVideoDecoder final : public VideoDecoder {
public:
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame);
    virtual ~FFmpegVideoDecoder() override;

    virtual DecoderErrorOr<void> receive_coded_data(AK::Duration timestamp, ReadonlyBytes coded_data) override;
    virtual DecoderErrorOr<NonnullOwnPtr<VideoFrame>> get_decoded_frame() override;

    virtual void flush() override;

private:
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
};

}
