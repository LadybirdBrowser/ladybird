/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoFramePool.h>

namespace Media::WindowsMediaFoundation {

// Decodes the codecs we cannot ship decoders for through the ones Windows provides as Media Foundation transforms. A
// transform can hand out a frame before it has finished decoding, so a thread of the decoder's own waits for each one
// to finish and hands it out.
class MEDIACODECS_API WindowsMediaFoundationVideoDecoder final : public VideoDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<WindowsMediaFoundationVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);

    virtual ~WindowsMediaFoundationVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_frame_pool->set_slot_freed_callback(move(callback)); }
    virtual void set_output_arrived_callback(Function<void()>) override;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target = {}) override;
    virtual void flush() override;

private:
    // These hold everything that names a Media Foundation type, so that the Windows headers stay out of this one.
    struct Transform;
    struct DecodedOutput;
    // Shared with the completion thread, which can outlive the decoder while it finishes waiting on a frame.
    class ThreadData;

    struct SubmittedFrame {
        i64 sample_time { 0 };
        AK::Duration timestamp;
        AK::Duration duration;
        DecodeIntent intent { DecodeIntent::Output };
    };

    WindowsMediaFoundationVideoDecoder(CodecID, NonnullOwnPtr<Transform>, NonnullRefPtr<ThreadData>, NonnullRefPtr<VideoFramePool>);

    DecoderErrorOr<void> apply_codec_configuration(ReadonlyBytes);
    DecoderErrorOr<ByteBuffer> annex_b_access_unit_for(CodedFrame const&);

    DecoderErrorOr<void> collect_available_outputs();
    DecoderErrorOr<void> negotiate_output_format();
    Optional<size_t> index_of_submitted_frame(i64 sample_time) const;
    void retire_submitted_frames_through(Optional<size_t> index);

    DecoderErrorOr<NonnullRefPtr<VideoFrame>> copy_output_into_frame_pool(DecodedOutput const&, CodingIndependentCodePoints const& container_cicp);

    CodecID const m_codec_id;
    NonnullOwnPtr<Transform> m_transform;
    NonnullRefPtr<ThreadData> m_thread_data;
    NonnullRefPtr<VideoFramePool> m_frame_pool;

    u8 m_nal_unit_length_size { 4 };
    // The transform never sees the configuration record, so its parameter sets are sent ahead of each keyframe.
    ByteBuffer m_annex_b_parameter_sets;

    // Ordered by sample time, which is how the transform hands frames back.
    Vector<SubmittedFrame> m_submitted_frames;
    bool m_draining { false };
    bool m_drained { false };
    // Input only resumes after a drain through a flush, and the transform expects the first sample after either to be
    // marked as following a gap.
    bool m_next_input_is_discontinuous { false };
};

}
