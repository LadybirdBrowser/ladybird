/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/Codecs/VP9.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/Export.h>
#include <LibMedia/Subsampling.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFramePool.h>
#include <LibMedia/VideoSurface.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>

namespace Media::VideoToolbox {

// The parameter sets a stream is configured from, tracked across the frames that carry them.
struct ParameterSetState;

// Decodes on the platform's media engine, which allocates the surfaces it decodes into and hands them back rather
// than filling ones we provide.
class MEDIA_API VideoToolboxVideoDecoder final : public VideoDecoder {
public:
    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<VideoToolboxVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);

    virtual ~VideoToolboxVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_surface_pool->set_slot_freed_callback(move(callback)); }

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp) override;
    virtual void flush() override;

private:
    struct Session;
    struct DecodedOutput {
        NonnullRefPtr<VideoSurface> surface;
        AK::Duration timestamp;
        AK::Duration duration;
        Gfx::IntSize size;
        u8 bit_depth { 0 };
        Subsampling subsampling;
        CodingIndependentCodePoints cicp;
    };

    VideoToolboxVideoDecoder(CodecID, NonnullRefPtr<VideoFrameSurfacePool>);

    DecoderErrorOr<void> ensure_session_for_frame(CodedFrame const&);

    void note_decode_completed(CodingIndependentCodePoints const& cicp, u64 generation, i32 status, void* image_buffer, AK::Duration timestamp, AK::Duration duration);

    void enqueue_decoded_output_while_locked(CodingIndependentCodePoints const& cicp, void* image_buffer, AK::Duration timestamp, AK::Duration duration);
    void note_decode_failure_while_locked(i32 status);
    void note_frame_left_the_media_engine_while_locked();

    void insert_output_in_presentation_order_while_locked(DecodedOutput&&);
    bool may_pull_frame_from_reorder_queue_while_locked() const;

    CodecID const m_codec_id;
    NonnullRefPtr<VideoFrameSurfacePool> m_surface_pool;
    // Codecs that configure a decoder from parameter sets carried in the stream track them here. The rest describe
    // their format in each frame, and leave this null.
    OwnPtr<ParameterSetState> m_parameter_set_state;
    OwnPtr<Session> m_session;
    bool m_reached_end_of_stream { false };

    // The media engine decodes on its own threads, so outputs arrive from outside this decoder's caller.
    mutable Sync::Mutex m_output_mutex;
    Sync::ConditionVariable m_output_arrived { m_output_mutex };
    Vector<DecodedOutput> m_outputs;
    u8 m_reorder_frame_count { 0 };
    Optional<DecoderError> m_decode_failure;

    // Only the caller's thread submits, so a read followed by an increment cannot exceed the limit; the media
    // engine's threads only ever retire slots. Relaxed suffices because the modification order of a single
    // location is total, so a retiring frame always observes its own submission.
    Atomic<size_t> m_frames_in_flight { 0 };
    Atomic<u64> m_generation { 0 };
};

}
