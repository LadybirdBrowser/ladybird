/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoDecoder.h>

#include "FFmpegForward.h"

namespace Media::FFmpeg {

class MEDIA_API FFmpegVideoDecoder final : public VideoDecoder {
public:
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame);
    virtual ~FFmpegVideoDecoder() override;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<VideoFrameMetadata> peek_next_output(CodingIndependentCodePoints const& container_cicp) override;
    virtual DecoderErrorOr<void> take_next_output_into(Gfx::YUVData&) override;

    virtual void flush() override;

private:
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
    bool m_has_pending_frame { false };
};

}
