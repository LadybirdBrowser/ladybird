/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/Codecs/VP9.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/Subsampling.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibMediaCodecs/VideoSurface.h>

namespace Media::VideoToolbox {

// What a stream has said about its format, tracked across the frames that say it.
struct FormatState;

// Decodes on the platform's media engine, which allocates the surfaces it decodes into and hands them back rather
// than filling ones we provide.
class MEDIACODECS_API VideoToolboxVideoDecoder final : public VideoDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<VideoToolboxVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);

    virtual ~VideoToolboxVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_surface_budget->set_storage_freed_callback(move(callback)); }
    virtual void set_output_arrived_callback(Function<void()> callback) override { m_output_arrived_callback = move(callback); }

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target = {}) override;
    virtual void flush() override;

private:
    struct Session;
    struct DecodedOutput {
        NonnullRefPtr<VideoSurface> surface;
        // Keeps the media engine from decoding over the surface, from the reorder window until the frame it goes out
        // in is released.
        NonnullRefPtr<VideoSurfaceContents> contents;
        AK::Duration timestamp;
        AK::Duration duration;
        Gfx::IntSize size;
        u8 bit_depth { 0 };
        Subsampling subsampling;
        CodingIndependentCodePoints cicp;
    };

    VideoToolboxVideoDecoder(CodecID, NonnullOwnPtr<FormatState>);

    DecoderErrorOr<void> ensure_session_for_frame(CodedFrame const&);

    void note_decode_completed(CodingIndependentCodePoints const& cicp, u64 generation, i32 status, void* image_buffer, AK::Duration timestamp, AK::Duration duration);

    void enqueue_decoded_output_while_locked(CodingIndependentCodePoints const& cicp, void* image_buffer, AK::Duration timestamp, AK::Duration duration);
    void note_decode_failure_while_locked(i32 status);
    void note_frame_left_the_media_engine_while_locked();

    void insert_output_in_presentation_order_while_locked(DecodedOutput&&);
    bool may_pull_frame_from_reorder_queue_while_locked() const;
    DecoderErrorOr<NonnullRefPtr<VideoFrame>> adopt_output_while_locked(CodingIndependentCodePoints const& container_cicp, size_t index);

    CodecID const m_codec_id;
    NonnullRefPtr<VideoSurfaceBudget> m_surface_budget { make_ref_counted<VideoSurfaceBudget>() };
    NonnullOwnPtr<FormatState> m_format_state;
    OwnPtr<Session> m_session;
    bool m_reached_end_of_stream { false };
    // ITU-T H.265 (07/2024), 8.1.3: the IRAP picture decoding starts at has NoRaslOutputFlag set, which gives the
    // RASL pictures associated with it PicOutputFlag equal to 0.
    bool m_awaiting_first_submitted_frame { true };
    bool m_discarding_open_gop_leading_pictures { false };

    // The media engine decodes on its own threads, so outputs arrive from outside this decoder's caller.
    mutable Mutex m_output_mutex;
    ConditionVariable m_output_arrived { m_output_mutex };
    bool m_caller_is_waiting_for_output { false };
    Function<void()> m_output_arrived_callback;
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
