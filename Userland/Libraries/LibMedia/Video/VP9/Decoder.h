/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Queue.h>
#include <AK/Span.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>

#include "Parser.h"

namespace Media::Video::VP9 {

class Decoder final : public VideoDecoder {
    friend class Parser;

public:
    Decoder();
    ~Decoder() override { }
    /* (8.1) General */
    DecoderErrorOr<void> receive_sample(Duration timestamp, ReadonlyBytes) override;

    DecoderErrorOr<NonnullOwnPtr<VideoFrame>> get_decoded_frame() override;

    void flush() override;

private:
    typedef i32 Intermediate;

    // Based on the maximum size resulting from num_4x4_blocks_wide_lookup.
    static constexpr size_t maximum_block_dimensions = 64ULL;
    static constexpr size_t maximum_block_size = maximum_block_dimensions * maximum_block_dimensions;
    // Based on the maximum for TXSize.
    static constexpr size_t maximum_transform_size = 32ULL * 32ULL;

    DecoderErrorOr<void> decode_frame(Duration timestamp, ReadonlyBytes);
    template<typename T>
    DecoderErrorOr<void> create_video_frame(Duration timestamp, FrameContext const&);

    DecoderErrorOr<void> allocate_buffers(FrameContext const&);
    Vector<u16>& get_output_buffer(u8 plane);

    /* (8.4) Probability Adaptation Process */
    u8 merge_prob(u8 pre_prob, u32 count_0, u32 count_1, u8 count_sat, u8 max_update_factor);
    u32 merge_probs(int const* tree, int index, u8* probs, u32* counts, u8 count_sat, u8 max_update_factor);
    DecoderErrorOr<void> adapt_coef_probs(FrameContext const&);
    DecoderErrorOr<void> adapt_non_coef_probs(FrameContext const&);
    void adapt_probs(int const* tree, u8* probs, u32* counts);
    u8 adapt_prob(u8 prob, u32 counts[2]);

    /* (8.5) Prediction Processes */
    // (8.5.1) Intra prediction process
    DecoderErrorOr<void> predict_intra(u8 plane, BlockContext const& block_context, u32 x, u32 y, bool have_left, bool have_above, bool not_on_right, TransformSize transform_size, u32 block_index);

    DecoderErrorOr<void> prepare_referenced_frame(Gfx::Size<u32> frame_size, u8 reference_frame_index);

    // (8.5.1) Inter prediction process
    DecoderErrorOr<void> predict_inter(u8 plane, BlockContext const& block_context, u32 x, u32 y, u32 width, u32 height, u32 block_index);
    // (8.5.2.1) Motion vector selection process
    MotionVector select_motion_vector(u8 plane, BlockContext const&, ReferenceIndex, u32 block_index);
    // (8.5.2.2) Motion vector clamping process
    MotionVector clamp_motion_vector(u8 plane, BlockContext const&, u32 block_row, u32 block_column, MotionVector vector);
    // From (8.5.1) Inter prediction process, steps 2-5
    DecoderErrorOr<void> predict_inter_block(u8 plane, BlockContext const&, ReferenceIndex, u32 block_row, u32 block_column, u32 x, u32 y, u32 width, u32 height, u32 block_index, Span<u16> block_buffer);

    /* (8.6) Reconstruction and Dequantization */

    // Returns the quantizer index for the current block
    static u8 get_base_quantizer_index(SegmentFeatureStatus alternative_quantizer_feature, bool should_use_absolute_segment_base_quantizer, u8 base_quantizer_index);
    // Returns the quantizer value for the dc coefficient for a particular plane
    static u16 get_dc_quantizer(u8 bit_depth, u8 base, i8 delta);
    // Returns the quantizer value for the ac coefficient for a particular plane
    static u16 get_ac_quantizer(u8 bit_depth, u8 base, i8 delta);

    // (8.6.2) Reconstruct process
    DecoderErrorOr<void> reconstruct(u8 plane, BlockContext const&, u32 transform_block_x, u32 transform_block_y, TransformSize transform_block_size, TransformSet);
    template<u8 log2_of_block_size>
    DecoderErrorOr<void> reconstruct_templated(u8 plane, BlockContext const&, u32 transform_block_x, u32 transform_block_y, TransformSet);

    // (8.7) Inverse transform process
    template<u8 log2_of_block_size>
    DecoderErrorOr<void> inverse_transform_2d(BlockContext const&, Span<Intermediate> dequantized, TransformSet);

    // (8.7.1) 1D Transforms
    // (8.7.1.1) Butterfly functions

    inline i32 cos64(u8 angle);
    inline i32 sin64(u8 angle);
    // The function B( a, b, angle, 0 ) performs a butterfly rotation.
    inline void butterfly_rotation_in_place(Span<Intermediate> data, size_t index_a, size_t index_b, u8 angle, bool flip);
    // The function H( a, b, 0 ) performs a Hadamard rotation.
    inline void hadamard_rotation_in_place(Span<Intermediate> data, size_t index_a, size_t index_b, bool flip);
    // The function SB( a, b, angle, 0 ) performs a butterfly rotation.
    // Spec defines the source as array T, and the destination array as S.
    template<typename S, typename D>
    inline void butterfly_rotation(Span<S> source, Span<D> destination, size_t index_a, size_t index_b, u8 angle, bool flip);
    // The function SH( a, b ) performs a Hadamard rotation and rounding.
    // Spec defines the source array as S, and the destination array as T.
    template<typename S, typename D>
    inline void hadamard_rotation(Span<S> source, Span<D> destination, size_t index_a, size_t index_b);

    // (8.7.1.10) This process does an in-place Walsh-Hadamard transform of the array T (of length 4).
    inline DecoderErrorOr<void> inverse_walsh_hadamard_transform(Span<Intermediate> data, u8 log2_of_block_size, u8 shift);

    // (8.7.1.2) Inverse DCT array permutation process
    template<u8 log2_of_block_size>
    inline DecoderErrorOr<void> inverse_discrete_cosine_transform_array_permutation(Span<Intermediate> data);
    // (8.7.1.3) Inverse DCT process
    template<u8 log2_of_block_size>
    inline DecoderErrorOr<void> inverse_discrete_cosine_transform(Span<Intermediate> data);

    // (8.7.1.4) This process performs the in-place permutation of the array T of length 2 n which is required as the first step of
    // the inverse ADST.
    template<u8 log2_of_block_size>
    inline void inverse_asymmetric_discrete_sine_transform_input_array_permutation(Span<Intermediate> data);
    // (8.7.1.5) This process performs the in-place permutation of the array T of length 2 n which is required before the final
    // step of the inverse ADST.
    template<u8 log2_of_block_size>
    inline void inverse_asymmetric_discrete_sine_transform_output_array_permutation(Span<Intermediate> data);

    // (8.7.1.6) This process does an in-place transform of the array T to perform an inverse ADST.
    inline void inverse_asymmetric_discrete_sine_transform_4(Span<Intermediate> data);
    // (8.7.1.7) This process does an in-place transform of the array T using a higher precision array S for intermediate
    // results.
    inline DecoderErrorOr<void> inverse_asymmetric_discrete_sine_transform_8(Span<Intermediate> data);
    // (8.7.1.8) This process does an in-place transform of the array T using a higher precision array S for intermediate
    // results.
    inline DecoderErrorOr<void> inverse_asymmetric_discrete_sine_transform_16(Span<Intermediate> data);
    // (8.7.1.9) This process performs an in-place inverse ADST process on the array T of size 2 n for 2 ≤ n ≤ 4.
    template<u8 log2_of_block_size>
    inline DecoderErrorOr<void> inverse_asymmetric_discrete_sine_transform(Span<Intermediate> data);

    /* (8.10) Reference Frame Update Process */
    DecoderErrorOr<void> update_reference_frames(FrameContext const&);

    NonnullOwnPtr<Parser> m_parser;

    Vector<u16> m_output_buffers[3];

    Queue<NonnullOwnPtr<VideoFrame>, 1> m_video_frame_queue;
};

}
