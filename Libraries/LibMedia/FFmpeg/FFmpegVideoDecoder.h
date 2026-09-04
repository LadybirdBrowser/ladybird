/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFramePool.h>

#include "FFmpegForward.h"

namespace Media::FFmpeg {

class MEDIA_API FFmpegVideoDecoder final : public VideoDecoder {
public:
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, NonnullRefPtr<VideoFramePool> frame_pool);
    virtual ~FFmpegVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_frame_pool->set_slot_freed_callback(move(callback)); }

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp) override;

    virtual void flush() override;

private:
    DecoderErrorOr<void> copy_pending_frame_into(Gfx::YUVData&);

    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
    NonnullRefPtr<VideoFramePool> m_frame_pool;
    bool m_has_pending_frame { false };

    HashTable<i64> m_reference_only_presentation_timestamps;
};

}
