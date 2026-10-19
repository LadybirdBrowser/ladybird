/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoFramePool.h>

#include <LibMediaCodecs/FFmpeg/FFmpegForward.h>
#include <LibMediaCodecs/FFmpeg/VideoFramesAwaitingOutput.h>

namespace Media::FFmpeg {

class MEDIACODECS_API FFmpegVideoDecoder final : public VideoDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(FFmpegFunctions const&, ParsedCodec const&);
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(FFmpegFunctions const&, CodecID, ReadonlyBytes codec_initialization_data);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(FFmpegFunctions const&, AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, NonnullRefPtr<VideoFramePool> frame_pool);
    virtual ~FFmpegVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_frame_pool->set_slot_freed_callback(move(callback)); }
    virtual void set_output_arrived_callback(Function<void()>) override { }

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target = {}) override;

    virtual void flush() override;

private:
    DecoderErrorOr<void> copy_pending_frame_into(Gfx::YUVData&);

    FFmpegFunctions const& m_functions;
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
    NonnullRefPtr<VideoFramePool> m_frame_pool;
    Optional<VideoFramesAwaitingOutput::Frame> m_pending_frame;
    VideoFramesAwaitingOutput m_frames_awaiting_output;
};

}
