/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
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
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(FFmpegFunctions const&, ParsedCodec const&);
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(FFmpegFunctions const&, CodecID, ReadonlyBytes codec_initialization_data);
    static DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);
    FFmpegVideoDecoder(FFmpegFunctions const&, AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, NonnullRefPtr<VideoFramePool> frame_pool);
    virtual ~FFmpegVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_frame_pool->set_slot_freed_callback(move(callback)); }

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target = {}) override;

    virtual void flush() override;

private:
    struct InFlightFrame {
        AK::Duration timestamp;
        AK::Duration duration;
        DecodeIntent intent;
    };

    DecoderErrorOr<void> copy_pending_frame_into(Gfx::YUVData&);
    i64 codec_context_option(char const* name) const;

    FFmpegFunctions const& m_functions;
    AVCodecContext* m_codec_context;
    AVPacket* m_packet;
    AVFrame* m_frame;
    NonnullRefPtr<VideoFramePool> m_frame_pool;
    Optional<InFlightFrame> m_pending_frame;

    // The decoder returns a packet's timestamp with the frame it produces, so a key stands in for it.
    u64 m_next_frame_key { 0 };
    HashMap<u64, InFlightFrame> m_frames_in_flight;
};

}
