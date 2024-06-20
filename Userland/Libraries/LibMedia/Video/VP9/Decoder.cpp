/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IntegralMath.h>
#include <AK/TypedTransfer.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#include "Context.h"
#include "Decoder.h"
#include "Utilities.h"

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

namespace Media::Video::VP9 {

Decoder::Decoder()
    : m_parser(make<Parser>(*this))
{
}

DecoderErrorOr<void> Decoder::receive_sample(Duration timestamp, ReadonlyBytes chunk_data)
{
    auto superframe_sizes = m_parser->parse_superframe_sizes(chunk_data);

    if (superframe_sizes.is_empty()) {
        return decode_frame(timestamp, chunk_data);
    }

    size_t offset = 0;

    for (auto superframe_size : superframe_sizes) {
        auto checked_size = Checked<size_t>(superframe_size);
        checked_size += offset;
        if (checked_size.has_overflow() || checked_size.value() > chunk_data.size())
            return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Superframe size invalid"sv);
        auto frame_data = chunk_data.slice(offset, superframe_size);
        TRY(decode_frame(timestamp, frame_data));
        offset = checked_size.value();
    }

    return {};
}

DecoderErrorOr<void> Decoder::decode_frame(Duration timestamp, ReadonlyBytes frame_data)
{
    // 1. The syntax elements for the coded frame are extracted as specified in sections 6 and 7. The syntax
    // tables include function calls indicating when the block decode processes should be triggered.
    auto frame_context = TRY(m_parser->parse_frame(frame_data));

    // 2. If loop_filter_level is not equal to 0, the loop filter process as specified in section 8.8 is invoked once the
    // coded frame has been decoded.
    // FIXME: Implement loop filtering.

    // 3. If all of the following conditions are true, PrevSegmentIds[ row ][ col ] is set equal to
    // SegmentIds[ row ][ col ] for row = 0..MiRows-1, for col = 0..MiCols-1:
    // − show_existing_frame is equal to 0,
    // − segmentation_enabled is equal to 1,
    // − segmentation_update_map is equal to 1.
    // This is handled by update_reference_frames.

    // 4. The output process as specified in section 8.9 is invoked.
    if (frame_context.shows_a_frame()) {
        switch (frame_context.color_config.bit_depth) {
        case 8:
            TRY(create_video_frame<u8>(timestamp, frame_context));
            break;
        case 10:
        case 12:
            TRY(create_video_frame<u16>(timestamp, frame_context));
            break;
        }
    }

    // 5. The reference frame update process as specified in section 8.10 is invoked.
    TRY(update_reference_frames(frame_context));
    return {};
}

inline CodingIndependentCodePoints get_cicp_color_space(FrameContext const& frame_context)
{
    ColorPrimaries color_primaries;
    TransferCharacteristics transfer_characteristics;
    MatrixCoefficients matrix_coefficients;

    switch (frame_context.color_config.color_space) {
    case ColorSpace::Unknown:
        color_primaries = ColorPrimaries::Unspecified;
        transfer_characteristics = TransferCharacteristics::Unspecified;
        matrix_coefficients = MatrixCoefficients::Unspecified;
        break;
    case ColorSpace::Bt601:
        color_primaries = ColorPrimaries::BT601;
        transfer_characteristics = TransferCharacteristics::BT601;
        matrix_coefficients = MatrixCoefficients::BT601;
        break;
    case ColorSpace::Bt709:
        color_primaries = ColorPrimaries::BT709;
        transfer_characteristics = TransferCharacteristics::BT709;
        matrix_coefficients = MatrixCoefficients::BT709;
        break;
    case ColorSpace::Smpte170:
        // https://www.kernel.org/doc/html/v4.9/media/uapi/v4l/pixfmt-007.html#colorspace-smpte-170m-v4l2-colorspace-smpte170m
        color_primaries = ColorPrimaries::BT601;
        transfer_characteristics = TransferCharacteristics::BT709;
        matrix_coefficients = MatrixCoefficients::BT601;
        break;
    case ColorSpace::Smpte240:
        color_primaries = ColorPrimaries::SMPTE240;
        transfer_characteristics = TransferCharacteristics::SMPTE240;
        matrix_coefficients = MatrixCoefficients::SMPTE240;
        break;
    case ColorSpace::Bt2020:
        color_primaries = ColorPrimaries::BT2020;
        // Bit depth doesn't actually matter to our transfer functions since we
        // convert in floats of range 0-1 (for now?), but just for correctness set
        // the TC to match the bit depth here.
        if (frame_context.color_config.bit_depth == 12)
            transfer_characteristics = TransferCharacteristics::BT2020BitDepth12;
        else if (frame_context.color_config.bit_depth == 10)
            transfer_characteristics = TransferCharacteristics::BT2020BitDepth10;
        else
            transfer_characteristics = TransferCharacteristics::BT709;
        matrix_coefficients = MatrixCoefficients::BT2020NonConstantLuminance;
        break;
    case ColorSpace::RGB:
        color_primaries = ColorPrimaries::BT709;
        transfer_characteristics = TransferCharacteristics::Linear;
        matrix_coefficients = MatrixCoefficients::Identity;
        break;
    case ColorSpace::Reserved:
        VERIFY_NOT_REACHED();
        break;
    }

    return { color_primaries, transfer_characteristics, matrix_coefficients, frame_context.color_config.color_range };
}

template<typename T>
DecoderErrorOr<void> Decoder::create_video_frame(Duration timestamp, FrameContext const& frame_context)
{
    // (8.9) Output process

    // FIXME: If show_existing_frame is set, output from FrameStore[frame_to_show_map_index] here instead.
    if (frame_context.shows_existing_frame()) {
        dbgln("FIXME: Show an existing reference frame.");
    }

    // FIXME: The math isn't entirely accurate to spec. output_uv_size is probably incorrect for certain
    //        sizes, as the spec seems to prefer that the halved sizes be ceiled.
    u32 decoded_y_width = frame_context.decoded_size(false).width();
    auto decoded_uv_width = frame_context.decoded_size(true).width();

    Subsampling subsampling { frame_context.color_config.subsampling_x, frame_context.color_config.subsampling_y };
    auto output_y_size = frame_context.size().to_type<size_t>();
    auto output_uv_size = subsampling.subsampled_size(output_y_size);

    auto frame = DECODER_TRY_ALLOC(SubsampledYUVFrame::try_create(
        timestamp,
        { output_y_size.width(), output_y_size.height() },
        frame_context.color_config.bit_depth, get_cicp_color_space(frame_context),
        subsampling));
    for (u32 plane = 0; plane < 3; plane++) {
        auto* buffer = frame->get_plane_data<T>(plane);
        auto decoded_width = plane == 0 ? decoded_y_width : decoded_uv_width;
        auto output_size = plane == 0 ? output_y_size : output_uv_size;
        auto const* decoded_buffer = get_output_buffer(plane).data();

        for (u32 row = 0; row < output_size.height(); row++) {
            for (u32 column = 0; column < output_size.width(); column++)
                buffer[row * output_size.width() + column] = static_cast<T>(decoded_buffer[row * decoded_width + column]);
        }
    }

    m_video_frame_queue.enqueue(move(frame));

    return {};
}

DecoderErrorOr<void> Decoder::allocate_buffers(FrameContext const& frame_context)
{
    for (size_t plane = 0; plane < 3; plane++) {
        auto size = frame_context.decoded_size(plane > 0);

        auto& output_buffer = get_output_buffer(plane);
        output_buffer.clear_with_capacity();
        DECODER_TRY_ALLOC(output_buffer.try_resize_and_keep_capacity(size.width() * size.height()));
    }
    return {};
}

Vector<u16>& Decoder::get_output_buffer(u8 plane)
{
    return m_output_buffers[plane];
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> Decoder::get_decoded_frame()
{
    if (m_video_frame_queue.is_empty())
        return DecoderError::format(DecoderErrorCategory::NeedsMoreInput, "No video frame in queue.");

    return m_video_frame_queue.dequeue();
}

void Decoder::flush()
{
    m_video_frame_queue.clear();
}

template<typename T>
static inline i32 rounded_right_shift(T value, u8 bits)
{
    value = (value + static_cast<T>(1u << (bits - 1u))) >> bits;
    return static_cast<i32>(value);
}

u8 Decoder::merge_prob(u8 pre_prob, u32 count_0, u32 count_1, u8 count_sat, u8 max_update_factor)
{
    auto total_decode_count = count_0 + count_1;
    u8 prob = 128;
    if (total_decode_count != 0) {
        prob = static_cast<u8>(clip_3(1u, 255u, (count_0 * 256 + (total_decode_count >> 1)) / total_decode_count));
    }
    auto count = min(total_decode_count, count_sat);
    auto factor = (max_update_factor * count) / count_sat;
    return rounded_right_shift(pre_prob * (256 - factor) + (prob * factor), 8);
}

u32 Decoder::merge_probs(int const* tree, int index, u8* probs, u32* counts, u8 count_sat, u8 max_update_factor)
{
    auto s = tree[index];
    auto left_count = (s <= 0) ? counts[-s] : merge_probs(tree, s, probs, counts, count_sat, max_update_factor);
    auto r = tree[index + 1];
    auto right_count = (r <= 0) ? counts[-r] : merge_probs(tree, r, probs, counts, count_sat, max_update_factor);
    probs[index >> 1] = merge_prob(probs[index >> 1], left_count, right_count, count_sat, max_update_factor);
    return left_count + right_count;
}

DecoderErrorOr<void> Decoder::adapt_coef_probs(FrameContext const& frame_context)
{
    u8 update_factor;
    if (!frame_context.is_inter_predicted() || m_parser->m_previous_frame_type != FrameType::KeyFrame)
        update_factor = 112;
    else
        update_factor = 128;

    for (size_t t = 0; t < 4; t++) {
        for (size_t i = 0; i < 2; i++) {
            for (size_t j = 0; j < 2; j++) {
                for (size_t k = 0; k < 6; k++) {
                    size_t max_l = (k == 0) ? 3 : 6;
                    for (size_t l = 0; l < max_l; l++) {
                        auto& coef_probs = m_parser->m_probability_tables->coef_probs()[t][i][j][k][l];
                        merge_probs(small_token_tree, 2, coef_probs,
                            frame_context.counter->m_counts_token[t][i][j][k][l],
                            24, update_factor);
                        merge_probs(binary_tree, 0, coef_probs,
                            frame_context.counter->m_counts_more_coefs[t][i][j][k][l],
                            24, update_factor);
                    }
                }
            }
        }
    }

    return {};
}

#define ADAPT_PROB_TABLE(name, size)                                     \
    do {                                                                 \
        for (size_t i = 0; i < (size); i++) {                            \
            auto table = probs.name##_prob();                            \
            table[i] = adapt_prob(table[i], counter.m_counts_##name[i]); \
        }                                                                \
    } while (0)

#define ADAPT_TREE(tree_name, prob_name, count_name, size)                                                 \
    do {                                                                                                   \
        for (size_t i = 0; i < (size); i++) {                                                              \
            adapt_probs(tree_name##_tree, probs.prob_name##_probs()[i], counter.m_counts_##count_name[i]); \
        }                                                                                                  \
    } while (0)

DecoderErrorOr<void> Decoder::adapt_non_coef_probs(FrameContext const& frame_context)
{
    auto& probs = *m_parser->m_probability_tables;
    auto& counter = *frame_context.counter;
    ADAPT_PROB_TABLE(is_inter, IS_INTER_CONTEXTS);
    ADAPT_PROB_TABLE(comp_mode, COMP_MODE_CONTEXTS);
    ADAPT_PROB_TABLE(comp_ref, REF_CONTEXTS);
    for (size_t i = 0; i < REF_CONTEXTS; i++) {
        for (size_t j = 0; j < 2; j++)
            probs.single_ref_prob()[i][j] = adapt_prob(probs.single_ref_prob()[i][j], counter.m_counts_single_ref[i][j]);
    }
    ADAPT_TREE(inter_mode, inter_mode, inter_mode, INTER_MODE_CONTEXTS);
    ADAPT_TREE(intra_mode, y_mode, intra_mode, BLOCK_SIZE_GROUPS);
    ADAPT_TREE(intra_mode, uv_mode, uv_mode, INTRA_MODES);
    ADAPT_TREE(partition, partition, partition, PARTITION_CONTEXTS);
    ADAPT_PROB_TABLE(skip, SKIP_CONTEXTS);
    if (frame_context.interpolation_filter == Switchable) {
        ADAPT_TREE(interp_filter, interp_filter, interp_filter, INTERP_FILTER_CONTEXTS);
    }
    if (frame_context.transform_mode == TransformMode::Select) {
        for (size_t i = 0; i < TX_SIZE_CONTEXTS; i++) {
            auto& tx_probs = probs.tx_probs();
            auto& tx_counts = counter.m_counts_tx_size;
            adapt_probs(tx_size_8_tree, tx_probs[Transform_8x8][i], tx_counts[Transform_8x8][i]);
            adapt_probs(tx_size_16_tree, tx_probs[Transform_16x16][i], tx_counts[Transform_16x16][i]);
            adapt_probs(tx_size_32_tree, tx_probs[Transform_32x32][i], tx_counts[Transform_32x32][i]);
        }
    }
    adapt_probs(mv_joint_tree, probs.mv_joint_probs(), counter.m_counts_mv_joint);
    for (size_t i = 0; i < 2; i++) {
        probs.mv_sign_prob()[i] = adapt_prob(probs.mv_sign_prob()[i], counter.m_counts_mv_sign[i]);
        adapt_probs(mv_class_tree, probs.mv_class_probs()[i], counter.m_counts_mv_class[i]);
        probs.mv_class0_bit_prob()[i] = adapt_prob(probs.mv_class0_bit_prob()[i], counter.m_counts_mv_class0_bit[i]);
        for (size_t j = 0; j < MV_OFFSET_BITS; j++)
            probs.mv_bits_prob()[i][j] = adapt_prob(probs.mv_bits_prob()[i][j], counter.m_counts_mv_bits[i][j]);
        for (size_t j = 0; j < CLASS0_SIZE; j++)
            adapt_probs(mv_fr_tree, probs.mv_class0_fr_probs()[i][j], counter.m_counts_mv_class0_fr[i][j]);
        adapt_probs(mv_fr_tree, probs.mv_fr_probs()[i], counter.m_counts_mv_fr[i]);
        if (frame_context.high_precision_motion_vectors_allowed) {
            probs.mv_class0_hp_prob()[i] = adapt_prob(probs.mv_class0_hp_prob()[i], counter.m_counts_mv_class0_hp[i]);
            probs.mv_hp_prob()[i] = adapt_prob(probs.mv_hp_prob()[i], counter.m_counts_mv_hp[i]);
        }
    }
    return {};
}

void Decoder::adapt_probs(int const* tree, u8* probs, u32* counts)
{
    merge_probs(tree, 0, probs, counts, COUNT_SAT, MAX_UPDATE_FACTOR);
}

u8 Decoder::adapt_prob(u8 prob, u32 counts[2])
{
    return merge_prob(prob, counts[0], counts[1], COUNT_SAT, MAX_UPDATE_FACTOR);
}

DecoderErrorOr<void> Decoder::predict_intra(u8 plane, BlockContext const& block_context, u32 x, u32 y, bool have_left, bool have_above, bool not_on_right, TransformSize tx_size, u32 block_index)
{
    auto& frame_buffer = get_output_buffer(plane);

    // 8.5.1 Intra prediction process

    // The intra prediction process is invoked for intra coded blocks to predict a part of the block corresponding to a
    // transform block. When the transform size is smaller than the block size, this process can be invoked multiple
    // times within a single block for the same plane, and the invocations are in raster order within the block.

    // The variable mode is specified by:
    //     1. If plane is greater than 0, mode is set equal to uv_mode.
    //     2. Otherwise, if MiSize is greater than or equal to BLOCK_8X8, mode is set equal to y_mode.
    //     3. Otherwise, mode is set equal to sub_modes[ blockIdx ].
    PredictionMode mode;
    if (plane > 0)
        mode = block_context.uv_prediction_mode;
    else if (block_context.size >= Block_8x8)
        mode = block_context.y_prediction_mode();
    else
        mode = block_context.sub_block_prediction_modes[block_index];

    // The variable log2Size specifying the base 2 logarithm of the width of the transform block is set equal to txSz + 2.
    u8 log2_of_block_size = tx_size + 2;
    // The variable size is set equal to 1 << log2Size.
    u8 block_size = 1 << log2_of_block_size;

    // The variable maxX is set equal to (MiCols * 8) - 1.
    // The variable maxY is set equal to (MiRows * 8) - 1.
    // If plane is greater than 0, then:
    //  − maxX is set equal to ((MiCols * 8) >> subsampling_x) - 1.
    //  − maxY is set equal to ((MiRows * 8) >> subsampling_y) - 1.
    auto output_size = block_context.frame_context.decoded_size(plane > 0);
    auto max_x = output_size.width() - 1;
    auto max_y = output_size.height() - 1;

    auto const frame_buffer_at = [&](u32 row, u32 column) -> u16& {
        return frame_buffer[row * output_size.width() + column];
    };

    // The array aboveRow[ i ] for i = 0..size-1 is specified by:
    //     ..
    // The array aboveRow[ i ] for i = size..2*size-1 is specified by:
    //     ..
    // The array aboveRow[ i ] for i = -1 is specified by:
    //     ..

    // NOTE: above_row is an array ranging from 0 to (2*block_size).
    //       There are three sections to the array:
    //           - [0]
    //           - [1 .. block_size]
    //           - [block_size + 1 .. block_size * 2]
    //       The array indices must be offset by 1 to accommodate index -1.
    Array<Intermediate, maximum_block_dimensions * 2 + 1> above_row;
    auto above_row_at = [&](i32 index) -> Intermediate& {
        return above_row[index + 1];
    };

    // NOTE: This value is pre-calculated since it is reused in spec below.
    //       Use this to replace spec text "(1<<(BitDepth-1))".
    Intermediate half_sample_value = (1 << (block_context.frame_context.color_config.bit_depth - 1));

    // The array aboveRow[ i ] for i = 0..size-1 is specified by:
    if (!have_above) {
        // 1. If haveAbove is equal to 0, aboveRow[ i ] is set equal to (1<<(BitDepth-1)) - 1.
        // FIXME: Use memset?
        for (auto i = 0u; i < block_size; i++)
            above_row_at(i) = half_sample_value - 1;
    } else {
        // 2. Otherwise, aboveRow[ i ] is set equal to CurrFrame[ plane ][ y-1 ][ Min(maxX, x+i) ].
        for (auto i = 0u; i < block_size; i++)
            above_row_at(i) = frame_buffer_at(y - 1, min(max_x, x + i));
    }

    // The array aboveRow[ i ] for i = size..2*size-1 is specified by:
    if (have_above && not_on_right && tx_size == Transform_4x4) {
        // 1. If haveAbove is equal to 1 and notOnRight is equal to 1 and txSz is equal to 0,
        //    aboveRow[ i ] is set equal to CurrFrame[ plane ][ y-1 ][ Min(maxX, x+i) ].
        for (auto i = block_size; i < block_size * 2; i++)
            above_row_at(i) = frame_buffer_at(y - 1, min(max_x, x + i));
    } else {
        // 2. Otherwise, aboveRow[ i ] is set equal to aboveRow[ size-1 ].
        for (auto i = block_size; i < block_size * 2; i++)
            above_row_at(i) = above_row_at(block_size - 1);
    }

    // The array aboveRow[ i ] for i = -1 is specified by:
    if (have_above && have_left) {
        // 1. If haveAbove is equal to 1 and haveLeft is equal to 1, aboveRow[ -1 ] is set equal to
        //    CurrFrame[ plane ][ y-1 ][ Min(maxX, x-1) ].
        above_row_at(-1) = frame_buffer_at(y - 1, min(max_x, x - 1));
    } else if (have_above) {
        // 2. Otherwise if haveAbove is equal to 1, aboveRow[ -1] is set equal to (1<<(BitDepth-1)) + 1.
        above_row_at(-1) = half_sample_value + 1;
    } else {
        // 3. Otherwise, aboveRow[ -1 ] is set equal to (1<<(BitDepth-1)) - 1
        above_row_at(-1) = half_sample_value - 1;
    }

    // The array leftCol[ i ] for i = 0..size-1 is specified by:
    Array<Intermediate, maximum_block_dimensions> left_column;
    if (have_left) {
        // − If haveLeft is equal to 1, leftCol[ i ] is set equal to CurrFrame[ plane ][ Min(maxY, y+i) ][ x-1 ].
        for (auto i = 0u; i < block_size; i++)
            left_column[i] = frame_buffer_at(min(max_y, y + i), x - 1);
    } else {
        // − Otherwise, leftCol[ i ] is set equal to (1<<(BitDepth-1)) + 1.
        for (auto i = 0u; i < block_size; i++)
            left_column[i] = half_sample_value + 1;
    }

    // A 2D array named pred containing the intra predicted samples is constructed as follows:
    Array<Intermediate, maximum_block_size> predicted_samples;
    auto const predicted_sample_at = [&](u32 row, u32 column) -> Intermediate& {
        return predicted_samples[row * block_size + column];
    };

    // FIXME: One of the two below should be a simple memcpy of 1D arrays.
    switch (mode) {
    case PredictionMode::VPred:
        // − If mode is equal to V_PRED, pred[ i ][ j ] is set equal to aboveRow[ j ] with j = 0..size-1 and i = 0..size-1
        // (each row of the block is filled with a copy of aboveRow).
        for (auto j = 0u; j < block_size; j++) {
            for (auto i = 0u; i < block_size; i++)
                predicted_sample_at(i, j) = above_row_at(j);
        }
        break;
    case PredictionMode::HPred:
        // − Otherwise if mode is equal to H_PRED, pred[ i ][ j ] is set equal to leftCol[ i ] with j = 0..size-1 and i =
        // 0..size-1 (each column of the block is filled with a copy of leftCol).
        for (auto j = 0u; j < block_size; j++) {
            for (auto i = 0u; i < block_size; i++)
                predicted_sample_at(i, j) = left_column[i];
        }
        break;
    case PredictionMode::D207Pred:
        // − Otherwise if mode is equal to D207_PRED, the following applies:
        // 1. pred[ size - 1 ][ j ] = leftCol[ size - 1] for j = 0..size-1
        for (auto j = 0u; j < block_size; j++)
            predicted_sample_at(block_size - 1, j) = left_column[block_size - 1];
        // 2. pred[ i ][ 0 ] = Round2( leftCol[ i ] + leftCol[ i + 1 ], 1 ) for i = 0..size-2
        for (auto i = 0u; i < block_size - 1u; i++)
            predicted_sample_at(i, 0) = rounded_right_shift(left_column[i] + left_column[i + 1], 1);
        // 3. pred[ i ][ 1 ] = Round2( leftCol[ i ] + 2 * leftCol[ i + 1 ] + leftCol[ i + 2 ], 2 ) for i = 0..size-3
        for (auto i = 0u; i < block_size - 2u; i++)
            predicted_sample_at(i, 1) = rounded_right_shift(left_column[i] + (2 * left_column[i + 1]) + left_column[i + 2], 2);
        // 4. pred[ size - 2 ][ 1 ] = Round2( leftCol[ size - 2 ] + 3 * leftCol[ size - 1 ], 2 )
        predicted_sample_at(block_size - 2, 1) = rounded_right_shift(left_column[block_size - 2] + (3 * left_column[block_size - 1]), 2);
        // 5. pred[ i ][ j ] = pred[ i + 1 ][ j - 2 ] for i = (size-2)..0, for j = 2..size-1
        // NOTE – In the last step i iterates in reverse order.
        for (auto i = block_size - 2u;;) {
            for (auto j = 2u; j < block_size; j++)
                predicted_sample_at(i, j) = predicted_sample_at(i + 1, j - 2);
            if (i == 0)
                break;
            i--;
        }
        break;
    case PredictionMode::D45Pred:
        // Otherwise if mode is equal to D45_PRED,
        // for i = 0..size-1, for j = 0..size-1.
        for (auto i = 0u; i < block_size; i++) {
            for (auto j = 0; j < block_size; j++) {
                // pred[ i ][ j ] is set equal to (i + j + 2 < size * 2) ?
                if (i + j + 2 < block_size * 2)
                    // Round2( aboveRow[ i + j ] + aboveRow[ i + j + 1 ] * 2 + aboveRow[ i + j + 2 ], 2 ) :
                    predicted_sample_at(i, j) = rounded_right_shift(above_row_at(i + j) + above_row_at(i + j + 1) * 2 + above_row_at(i + j + 2), 2);
                else
                    // aboveRow[ 2 * size - 1 ]
                    predicted_sample_at(i, j) = above_row_at(2 * block_size - 1);
            }
        }
        break;
    case PredictionMode::D63Pred:
        // Otherwise if mode is equal to D63_PRED,
        for (auto i = 0u; i < block_size; i++) {
            for (auto j = 0u; j < block_size; j++) {
                // i/2 + j
                auto row_index = (i / 2) + j;
                // pred[ i ][ j ] is set equal to (i & 1) ?
                if (i & 1)
                    // Round2( aboveRow[ i/2 + j ] + aboveRow[ i/2 + j + 1 ] * 2 + aboveRow[ i/2 + j + 2 ], 2 ) :
                    predicted_sample_at(i, j) = rounded_right_shift(above_row_at(row_index) + above_row_at(row_index + 1) * 2 + above_row_at(row_index + 2), 2);
                else
                    // Round2( aboveRow[ i/2 + j ] + aboveRow[ i/2 + j + 1 ], 1 ) for i = 0..size-1, for j = 0..size-1.
                    predicted_sample_at(i, j) = rounded_right_shift(above_row_at(row_index) + above_row_at(row_index + 1), 1);
            }
        }
        break;
    case PredictionMode::D117Pred:
        // Otherwise if mode is equal to D117_PRED, the following applies:
        // 1. pred[ 0 ][ j ] = Round2( aboveRow[ j - 1 ] + aboveRow[ j ], 1 ) for j = 0..size-1
        for (auto j = 0; j < block_size; j++)
            predicted_sample_at(0, j) = rounded_right_shift(above_row_at(j - 1) + above_row_at(j), 1);
        // 2. pred[ 1 ][ 0 ] = Round2( leftCol[ 0 ] + 2 * aboveRow[ -1 ] + aboveRow[ 0 ], 2 )
        predicted_sample_at(1, 0) = rounded_right_shift(left_column[0] + 2 * above_row_at(-1) + above_row_at(0), 2);
        // 3. pred[ 1 ][ j ] = Round2( aboveRow[ j - 2 ] + 2 * aboveRow[ j - 1 ] + aboveRow[ j ], 2 ) for j = 1..size-1
        for (auto j = 1; j < block_size; j++)
            predicted_sample_at(1, j) = rounded_right_shift(above_row_at(j - 2) + 2 * above_row_at(j - 1) + above_row_at(j), 2);
        // 4. pred[ 2 ][ 0 ] = Round2( aboveRow[ -1 ] + 2 * leftCol[ 0 ] + leftCol[ 1 ], 2 )
        predicted_sample_at(2, 0) = rounded_right_shift(above_row_at(-1) + 2 * left_column[0] + left_column[1], 2);
        // 5. pred[ i ][ 0 ] = Round2( leftCol[ i - 3 ] + 2 * leftCol[ i - 2 ] + leftCol[ i - 1 ], 2 ) for i = 3..size-1
        for (auto i = 3u; i < block_size; i++)
            predicted_sample_at(i, 0) = rounded_right_shift(left_column[i - 3] + 2 * left_column[i - 2] + left_column[i - 1], 2);
        // 6. pred[ i ][ j ] = pred[ i - 2 ][ j - 1 ] for i = 2..size-1, for j = 1..size-1
        for (auto i = 2u; i < block_size; i++) {
            for (auto j = 1u; j < block_size; j++)
                predicted_sample_at(i, j) = predicted_sample_at(i - 2, j - 1);
        }
        break;
    case PredictionMode::D135Pred:
        // Otherwise if mode is equal to D135_PRED, the following applies:
        // 1. pred[ 0 ][ 0 ] = Round2( leftCol[ 0 ] + 2 * aboveRow[ -1 ] + aboveRow[ 0 ], 2 )
        predicted_sample_at(0, 0) = rounded_right_shift(left_column[0] + 2 * above_row_at(-1) + above_row_at(0), 2);
        // 2. pred[ 0 ][ j ] = Round2( aboveRow[ j - 2 ] + 2 * aboveRow[ j - 1 ] + aboveRow[ j ], 2 ) for j = 1..size-1
        for (auto j = 1; j < block_size; j++)
            predicted_sample_at(0, j) = rounded_right_shift(above_row_at(j - 2) + 2 * above_row_at(j - 1) + above_row_at(j), 2);
        // 3. pred[ 1 ][ 0 ] = Round2( aboveRow [ -1 ] + 2 * leftCol[ 0 ] + leftCol[ 1 ], 2 ) for i = 1..size-1
        predicted_sample_at(1, 0) = rounded_right_shift(above_row_at(-1) + 2 * left_column[0] + left_column[1], 2);
        // 4. pred[ i ][ 0 ] = Round2( leftCol[ i - 2 ] + 2 * leftCol[ i - 1 ] + leftCol[ i ], 2 ) for i = 2..size-1
        for (auto i = 2u; i < block_size; i++)
            predicted_sample_at(i, 0) = rounded_right_shift(left_column[i - 2] + 2 * left_column[i - 1] + left_column[i], 2);
        // 5. pred[ i ][ j ] = pred[ i - 1 ][ j - 1 ] for i = 1..size-1, for j = 1..size-1
        for (auto i = 1u; i < block_size; i++) {
            for (auto j = 1; j < block_size; j++)
                predicted_sample_at(i, j) = predicted_sample_at(i - 1, j - 1);
        }
        break;
    case PredictionMode::D153Pred:
        // Otherwise if mode is equal to D153_PRED, the following applies:
        // 1. pred[ 0 ][ 0 ] = Round2( leftCol[ 0 ] + aboveRow[ -1 ], 1 )
        predicted_sample_at(0, 0) = rounded_right_shift(left_column[0] + above_row_at(-1), 1);
        // 2. pred[ i ][ 0 ] = Round2( leftCol[ i - 1] + leftCol[ i ], 1 ) for i = 1..size-1
        for (auto i = 1u; i < block_size; i++)
            predicted_sample_at(i, 0) = rounded_right_shift(left_column[i - 1] + left_column[i], 1);
        // 3. pred[ 0 ][ 1 ] = Round2( leftCol[ 0 ] + 2 * aboveRow[ -1 ] + aboveRow[ 0 ], 2 )
        predicted_sample_at(0, 1) = rounded_right_shift(left_column[0] + 2 * above_row_at(-1) + above_row_at(0), 2);
        // 4. pred[ 1 ][ 1 ] = Round2( aboveRow[ -1 ] + 2 * leftCol [ 0 ] + leftCol [ 1 ], 2 )
        predicted_sample_at(1, 1) = rounded_right_shift(above_row_at(-1) + 2 * left_column[0] + left_column[1], 2);
        // 5. pred[ i ][ 1 ] = Round2( leftCol[ i - 2 ] + 2 * leftCol[ i - 1 ] + leftCol[ i ], 2 ) for i = 2..size-1
        for (auto i = 2u; i < block_size; i++)
            predicted_sample_at(i, 1) = rounded_right_shift(left_column[i - 2] + 2 * left_column[i - 1] + left_column[i], 2);
        // 6. pred[ 0 ][ j ] = Round2( aboveRow[ j - 3 ] + 2 * aboveRow[ j - 2 ] + aboveRow[ j - 1 ], 2 ) for j = 2..size-1
        for (auto j = 2; j < block_size; j++)
            predicted_sample_at(0, j) = rounded_right_shift(above_row_at(j - 3) + 2 * above_row_at(j - 2) + above_row_at(j - 1), 2);
        // 7. pred[ i ][ j ] = pred[ i - 1 ][ j - 2 ] for i = 1..size-1, for j = 2..size-1
        for (auto i = 1u; i < block_size; i++) {
            for (auto j = 2u; j < block_size; j++)
                predicted_sample_at(i, j) = predicted_sample_at(i - 1, j - 2);
        }
        break;
    case PredictionMode::TmPred:
        // Otherwise if mode is equal to TM_PRED,
        // pred[ i ][ j ] is set equal to Clip1( aboveRow[ j ] + leftCol[ i ] - aboveRow[ -1 ] )
        // for i = 0..size-1, for j = 0..size-1.
        for (auto i = 0u; i < block_size; i++) {
            for (auto j = 0u; j < block_size; j++)
                predicted_sample_at(i, j) = clip_1(block_context.frame_context.color_config.bit_depth, above_row_at(j) + left_column[i] - above_row_at(-1));
        }
        break;
    case PredictionMode::DcPred: {
        Intermediate average = 0;

        if (have_left && have_above) {
            // Otherwise if mode is equal to DC_PRED and haveLeft is equal to 1 and haveAbove is equal to 1,
            // The variable avg (the average of the samples in union of aboveRow and leftCol)
            // is specified as follows:
            // sum = 0
            // for ( k = 0; k < size; k++ ) {
            //     sum += leftCol[ k ]
            //     sum += aboveRow[ k ]
            // }
            // avg = (sum + size) >> (log2Size + 1)
            Intermediate sum = 0;
            for (auto k = 0u; k < block_size; k++) {
                sum += left_column[k];
                sum += above_row_at(k);
            }
            average = (sum + block_size) >> (log2_of_block_size + 1);
        } else if (have_left && !have_above) {
            // Otherwise if mode is equal to DC_PRED and haveLeft is equal to 1 and haveAbove is equal to 0,
            // The variable leftAvg is specified as follows:
            // sum = 0
            // for ( k = 0; k < size; k++ ) {
            //     sum += leftCol[ k ]
            // }
            // leftAvg = (sum + (1 << (log2Size - 1) ) ) >> log2Size
            Intermediate sum = 0;
            for (auto k = 0u; k < block_size; k++)
                sum += left_column[k];
            average = (sum + (1 << (log2_of_block_size - 1))) >> log2_of_block_size;
        } else if (!have_left && have_above) {
            // Otherwise if mode is equal to DC_PRED and haveLeft is equal to 0 and haveAbove is equal to 1,
            // The variable aboveAvg is specified as follows:
            // sum = 0
            // for ( k = 0; k < size; k++ ) {
            // sum += aboveRow[ k ]
            // }
            // aboveAvg = (sum + (1 << (log2Size - 1) ) ) >> log2Size
            Intermediate sum = 0;
            for (auto k = 0u; k < block_size; k++)
                sum += above_row_at(k);
            average = (sum + (1 << (log2_of_block_size - 1))) >> log2_of_block_size;
        } else {
            // Otherwise (mode is DC_PRED),
            // pred[ i ][ j ] is set equal to 1<<(BitDepth - 1) with i = 0..size-1 and j = 0..size-1.
            average = 1 << (block_context.frame_context.color_config.bit_depth - 1);
        }

        // pred[ i ][ j ] is set equal to avg with i = 0..size-1 and j = 0..size-1.
        for (auto i = 0u; i < block_size; i++) {
            for (auto j = 0u; j < block_size; j++)
                predicted_sample_at(i, j) = average;
        }
        break;
    }
    default:
        dbgln("Unknown prediction mode {}", static_cast<u8>(mode));
        VERIFY_NOT_REACHED();
    }

    // The current frame is updated as follows:
    // − CurrFrame[ plane ][ y + i ][ x + j ] is set equal to pred[ i ][ j ] for i = 0..size-1 and j = 0..size-1.
    auto width_in_frame_buffer = min(static_cast<u32>(block_size), max_x - x + 1);
    auto height_in_frame_buffer = min(static_cast<u32>(block_size), max_y - y + 1);

    for (auto i = 0u; i < height_in_frame_buffer; i++) {
        for (auto j = 0u; j < width_in_frame_buffer; j++)
            frame_buffer_at(y + i, x + j) = predicted_sample_at(i, j);
    }

    return {};
}

MotionVector Decoder::select_motion_vector(u8 plane, BlockContext const& block_context, ReferenceIndex reference_index, u32 block_index)
{
    // The inputs to this process are:
    // − a variable plane specifying which plane is being predicted,
    // − a variable refList specifying that we should select the motion vector from BlockMvs[ refList ],
    // − a variable blockIdx, specifying how much of the block has already been predicted in units of 4x4 samples.
    // The output of this process is a 2 element array called mv containing the motion vector for this block.

    // The purpose of this process is to find the motion vector for this block. Motion vectors are specified for each
    // luma block, but a chroma block may cover more than one luma block due to subsampling. In this case, an
    // average motion vector is constructed for the chroma block.

    // The functions round_mv_comp_q2 and round_mv_comp_q4 perform division with rounding to the nearest
    // integer and are specified as:
    auto round_mv_comp_q2 = [&](MotionVector in) {
        // return (value < 0 ? value - 1 : value + 1) / 2
        return MotionVector {
            (in.row() < 0 ? in.row() - 1 : in.row() + 1) / 2,
            (in.column() < 0 ? in.column() - 1 : in.column() + 1) / 2
        };
    };
    auto round_mv_comp_q4 = [&](MotionVector in) {
        // return (value < 0 ? value - 2 : value + 2) / 4
        return MotionVector {
            (in.row() < 0 ? in.row() - 2 : in.row() + 2) / 4,
            (in.column() < 0 ? in.column() - 2 : in.column() + 2) / 4
        };
    };

    auto vectors = block_context.sub_block_motion_vectors;

    // The motion vector array mv is derived as follows:
    // − If plane is equal to 0, or MiSize is greater than or equal to BLOCK_8X8, mv is set equal to
    // BlockMvs[ refList ][ blockIdx ].
    if (plane == 0 || block_context.size >= Block_8x8)
        return vectors[block_index][reference_index];
    // − Otherwise, if subsampling_x is equal to 0 and subsampling_y is equal to 0, mv is set equal to
    // BlockMvs[ refList ][ blockIdx ].
    if (!block_context.frame_context.color_config.subsampling_x && !block_context.frame_context.color_config.subsampling_y)
        return vectors[block_index][reference_index];
    // − Otherwise, if subsampling_x is equal to 0 and subsampling_y is equal to 1, mv[ comp ] is set equal to
    // round_mv_comp_q2( BlockMvs[ refList ][ blockIdx ][ comp ] + BlockMvs[ refList ][ blockIdx + 2 ][ comp ] )
    // for comp = 0..1.
    if (!block_context.frame_context.color_config.subsampling_x && block_context.frame_context.color_config.subsampling_y)
        return round_mv_comp_q2(vectors[block_index][reference_index] + vectors[block_index + 2][reference_index]);
    // − Otherwise, if subsampling_x is equal to 1 and subsampling_y is equal to 0, mv[ comp ] is set equal to
    // round_mv_comp_q2( BlockMvs[ refList ][ blockIdx ][ comp ] + BlockMvs[ refList ][ blockIdx + 1 ][ comp ] )
    // for comp = 0..1.
    if (block_context.frame_context.color_config.subsampling_x && !block_context.frame_context.color_config.subsampling_y)
        return round_mv_comp_q2(vectors[block_index][reference_index] + vectors[block_index + 1][reference_index]);
    // − Otherwise, (subsampling_x is equal to 1 and subsampling_y is equal to 1), mv[ comp ] is set equal to
    // round_mv_comp_q4( BlockMvs[ refList ][ 0 ][ comp ] + BlockMvs[ refList ][ 1 ][ comp ] +
    // BlockMvs[ refList ][ 2 ][ comp ] + BlockMvs[ refList ][ 3 ][ comp ] ) for comp = 0..1.
    VERIFY(block_context.frame_context.color_config.subsampling_x && block_context.frame_context.color_config.subsampling_y);
    return round_mv_comp_q4(vectors[0][reference_index] + vectors[1][reference_index]
        + vectors[2][reference_index] + vectors[3][reference_index]);
}

MotionVector Decoder::clamp_motion_vector(u8 plane, BlockContext const& block_context, u32 block_row, u32 block_column, MotionVector vector)
{
    // FIXME: This function is named very similarly to Parser::clamp_mv. Rename one or the other?

    // The purpose of this process is to change the motion vector into the appropriate precision for the current plane
    // and to clamp motion vectors that go too far off the edge of the frame.
    // The variables sx and sy are set equal to the subsampling for the current plane as follows:
    // − If plane is equal to 0, sx is set equal to 0 and sy is set equal to 0.
    // − Otherwise, sx is set equal to subsampling_x and sy is set equal to subsampling_y.
    bool subsampling_x = plane > 0 ? block_context.frame_context.color_config.subsampling_x : false;
    bool subsampling_y = plane > 0 ? block_context.frame_context.color_config.subsampling_y : false;

    // The output array clampedMv is specified by the following steps:
    i32 blocks_high = num_8x8_blocks_high_lookup[block_context.size];
    // Casts must be done here to prevent subtraction underflow from wrapping the values.
    i32 mb_to_top_edge = -(static_cast<i32>(block_row * MI_SIZE) * 16) >> subsampling_y;
    i32 mb_to_bottom_edge = (((static_cast<i32>(block_context.frame_context.rows()) - blocks_high - static_cast<i32>(block_row)) * MI_SIZE) * 16) >> subsampling_y;

    i32 blocks_wide = num_8x8_blocks_wide_lookup[block_context.size];
    i32 mb_to_left_edge = -(static_cast<i32>(block_column * MI_SIZE) * 16) >> subsampling_x;
    i32 mb_to_right_edge = (((static_cast<i32>(block_context.frame_context.columns()) - blocks_wide - static_cast<i32>(block_column)) * MI_SIZE) * 16) >> subsampling_x;

    i32 subpel_left = (INTERP_EXTEND + ((blocks_wide * MI_SIZE) >> subsampling_x)) << SUBPEL_BITS;
    i32 subpel_right = subpel_left - SUBPEL_SHIFTS;
    i32 subpel_top = (INTERP_EXTEND + ((blocks_high * MI_SIZE) >> subsampling_y)) << SUBPEL_BITS;
    i32 subpel_bottom = subpel_top - SUBPEL_SHIFTS;
    return {
        clip_3(mb_to_top_edge - subpel_top, mb_to_bottom_edge + subpel_bottom, (2 * vector.row()) >> subsampling_y),
        clip_3(mb_to_left_edge - subpel_left, mb_to_right_edge + subpel_right, (2 * vector.column()) >> subsampling_x)
    };
}

static constexpr i32 maximum_scaled_step = 80;

DecoderErrorOr<void> Decoder::prepare_referenced_frame(Gfx::Size<u32> frame_size, u8 reference_frame_index)
{
    ReferenceFrame& reference_frame = m_parser->m_reference_frames[reference_frame_index];

    // 8.5.2.3 Motion vector scaling process
    // The inputs to this process are:
    // − a variable plane specifying which plane is being predicted,
    // − a variable refList specifying that we should scale to match reference frame ref_frame[ refList ],
    // − variables x and y specifying the location of the top left sample in the CurrFrame[ plane ] array of the region
    // to be predicted,
    // − a variable clampedMv specifying the clamped motion vector.
    // The outputs of this process are the variables startX and startY giving the reference block location in units of
    // 1/16 th of a sample, and variables xStep and yStep giving the step size in units of 1/16 th of a sample.
    // This process is responsible for computing the sampling locations in the reference frame based on the motion
    // vector. The sampling locations are also adjusted to compensate for any difference in the size of the reference
    // frame compared to the current frame.

    // It is a requirement of bitstream conformance that all the following conditions are satisfied:
    // − 2 * FrameWidth >= RefFrameWidth[ refIdx ]
    // − 2 * FrameHeight >= RefFrameHeight[ refIdx ]
    // − FrameWidth <= 16 * RefFrameWidth[ refIdx ]
    // − FrameHeight <= 16 * RefFrameHeight[ refIdx ]
    if (!reference_frame.is_valid())
        return DecoderError::format(DecoderErrorCategory::Corrupted, "Attempted to use reference frame {} that has not been saved", reference_frame_index);
    auto double_frame_size = frame_size.scaled(2);
    if (double_frame_size.width() < reference_frame.size.width() || double_frame_size.height() < reference_frame.size.height())
        return DecoderError::format(DecoderErrorCategory::Corrupted, "Inter frame size is too small relative to reference frame {}", reference_frame_index);
    if (!reference_frame.size.scaled(16).contains(frame_size))
        return DecoderError::format(DecoderErrorCategory::Corrupted, "Inter frame size is too large relative to reference frame {}", reference_frame_index);

    // FIXME: Convert all the operations in this function to vector operations supported by
    //        MotionVector.

    // A variable xScale is set equal to (RefFrameWidth[ refIdx ] << REF_SCALE_SHIFT) / FrameWidth.
    // A variable yScale is set equal to (RefFrameHeight[ refIdx ] << REF_SCALE_SHIFT) / FrameHeight.
    // (xScale and yScale specify the size of the reference frame relative to the current frame in units where 16 is
    // equivalent to the reference frame having the same size.)
    // NOTE: This spec note above seems to be incorrect. The 1:1 scale value would be 16,384.
    i32 x_scale = (reference_frame.size.width() << REF_SCALE_SHIFT) / frame_size.width();
    i32 y_scale = (reference_frame.size.height() << REF_SCALE_SHIFT) / frame_size.height();

    // The output variable stepX is set equal to (16 * xScale) >> REF_SCALE_SHIFT.
    // The output variable stepY is set equal to (16 * yScale) >> REF_SCALE_SHIFT.
    i32 scaled_step_x = (16 * x_scale) >> REF_SCALE_SHIFT;
    i32 scaled_step_y = (16 * y_scale) >> REF_SCALE_SHIFT;

    // 5. The block inter prediction process in section 8.5.2.4 is invoked with plane, refList, startX, startY, stepX,
    // stepY, w, h as inputs and the output is assigned to the 2D array preds[ refList ].

    // 8.5.2.4 Block inter prediction process
    // The inputs to this process are:
    // − a variable plane,
    // − a variable refList specifying that we should predict from ref_frame[ refList ],
    // − variables x and y giving the block location in units of 1/16 th of a sample,
    // − variables xStep and yStep giving the step size in units of 1/16 th of a sample. (These will be at most equal
    // to 80 due to the restrictions on scaling between reference frames.)
    VERIFY(scaled_step_x <= maximum_scaled_step && scaled_step_y <= maximum_scaled_step);
    // − variables w and h giving the width and height of the block in units of samples
    // The output from this process is the 2D array named pred containing inter predicted samples.

    reference_frame.x_scale = x_scale;
    reference_frame.y_scale = x_scale;
    reference_frame.scaled_step_x = scaled_step_x;
    reference_frame.scaled_step_y = scaled_step_y;

    return {};
}

DecoderErrorOr<void> Decoder::predict_inter_block(u8 plane, BlockContext const& block_context, ReferenceIndex reference_index, u32 block_row, u32 block_column, u32 x, u32 y, u32 width, u32 height, u32 block_index, Span<u16> block_buffer)
{
    VERIFY(width <= maximum_block_dimensions && height <= maximum_block_dimensions);
    // 2. The motion vector selection process in section 8.5.2.1 is invoked with plane, refList, blockIdx as inputs
    // and the output being the motion vector mv.
    auto motion_vector = select_motion_vector(plane, block_context, reference_index, block_index);

    // 3. The motion vector clamping process in section 8.5.2.2 is invoked with plane, mv as inputs and the output
    // being the clamped motion vector clampedMv
    auto clamped_vector = clamp_motion_vector(plane, block_context, block_row, block_column, motion_vector);

    // 4. The motion vector scaling process in section 8.5.2.3 is invoked with plane, refList, x, y, clampedMv as
    // inputs and the output being the initial location startX, startY, and the step sizes stepX, stepY.

    // 8.5.2.3 Motion vector scaling process
    // The inputs to this process are:
    // − a variable plane specifying which plane is being predicted,
    // − a variable refList specifying that we should scale to match reference frame ref_frame[ refList ],
    // − variables x and y specifying the location of the top left sample in the CurrFrame[ plane ] array of the region
    // to be predicted,
    // − a variable clampedMv specifying the clamped motion vector.
    // The outputs of this process are the variables startX and startY giving the reference block location in units of
    // 1/16 th of a sample, and variables xStep and yStep giving the step size in units of 1/16 th of a sample.
    // This process is responsible for computing the sampling locations in the reference frame based on the motion
    // vector. The sampling locations are also adjusted to compensate for any difference in the size of the reference
    // frame compared to the current frame.

    // NOTE: Some of this is done in advance by Decoder::prepare_referenced_frame().

    // A variable refIdx specifying which reference frame is being used is set equal to
    // ref_frame_idx[ ref_frame[ refList ] - LAST_FRAME ].
    auto reference_frame_index = block_context.frame_context.reference_frame_indices[block_context.reference_frame_types[reference_index] - ReferenceFrameType::LastFrame];
    auto const& reference_frame = m_parser->m_reference_frames[reference_frame_index];

    // Scale values range from 8192 to 262144.
    // 16384 = 1:1, higher values indicate the reference frame is larger than the current frame.
    auto x_scale = reference_frame.x_scale;
    auto y_scale = reference_frame.y_scale;

    // The amount of subpixels between each sample of this block. Non-16 values will cause the output to be scaled.
    auto scaled_step_x = reference_frame.scaled_step_x;
    auto scaled_step_y = reference_frame.scaled_step_y;

    // The variable baseX is set equal to (x * xScale) >> REF_SCALE_SHIFT.
    // The variable baseY is set equal to (y * yScale) >> REF_SCALE_SHIFT.
    // (baseX and baseY specify the location of the block in the reference frame if a zero motion vector is used).
    i32 base_x = (x * x_scale) >> REF_SCALE_SHIFT;
    i32 base_y = (y * y_scale) >> REF_SCALE_SHIFT;

    // The variable lumaX is set equal to (plane > 0) ? x << subsampling_x : x.
    // The variable lumaY is set equal to (plane > 0) ? y << subsampling_y : y.
    // (lumaX and lumaY specify the location of the block to be predicted in the current frame in units of luma
    // samples.)
    bool subsampling_x = plane > 0 ? block_context.frame_context.color_config.subsampling_x : false;
    bool subsampling_y = plane > 0 ? block_context.frame_context.color_config.subsampling_y : false;
    i32 luma_x = x << subsampling_x;
    i32 luma_y = y << subsampling_y;

    // The variable fracX is set equal to ( (16 * lumaX * xScale) >> REF_SCALE_SHIFT) & SUBPEL_MASK.
    // The variable fracY is set equal to ( (16 * lumaY * yScale) >> REF_SCALE_SHIFT) & SUBPEL_MASK.
    i32 frac_x = ((16 * luma_x * x_scale) >> REF_SCALE_SHIFT) & SUBPEL_MASK;
    i32 frac_y = ((16 * luma_y * y_scale) >> REF_SCALE_SHIFT) & SUBPEL_MASK;

    // The variable dX is set equal to ( (clampedMv[ 1 ] * xScale) >> REF_SCALE_SHIFT) + fracX.
    // The variable dY is set equal to ( (clampedMv[ 0 ] * yScale) >> REF_SCALE_SHIFT) + fracY.
    // (dX and dY specify a scaled motion vector.)
    i32 scaled_vector_x = ((clamped_vector.column() * x_scale) >> REF_SCALE_SHIFT) + frac_x;
    i32 scaled_vector_y = ((clamped_vector.row() * y_scale) >> REF_SCALE_SHIFT) + frac_y;

    // The output variable startX is set equal to (baseX << SUBPEL_BITS) + dX.
    // The output variable startY is set equal to (baseY << SUBPEL_BITS) + dY.
    i32 offset_scaled_block_x = (base_x << SUBPEL_BITS) + scaled_vector_x;
    i32 offset_scaled_block_y = (base_y << SUBPEL_BITS) + scaled_vector_y;

    // A variable ref specifying the reference frame contents is set equal to FrameStore[ refIdx ].
    auto& reference_frame_buffer = reference_frame.frame_planes[plane];
    auto reference_frame_width = Subsampling::subsampled_size(subsampling_x, reference_frame.size.width()) + MV_BORDER * 2;

    // The variable lastX is set equal to ( (RefFrameWidth[ refIdx ] + subX) >> subX) - 1.
    // The variable lastY is set equal to ( (RefFrameHeight[ refIdx ] + subY) >> subY) - 1.
    // (lastX and lastY specify the coordinates of the bottom right sample of the reference plane.)
    // Ad-hoc: These variables are not needed, since the reference frame is expanded to contain the samples that
    // may be referenced by motion vectors on the edge of the frame.

    // The sub-sample interpolation is effected via two one-dimensional convolutions. First a horizontal filter is used
    // to build up a temporary array, and then this array is vertically filtered to obtain the final prediction. The
    // fractional parts of the motion vectors determine the filtering process. If the fractional part is zero, then the
    // filtering is equivalent to a straight sample copy.
    // The filtering is applied as follows:

    constexpr auto sample_offset = 3;

    auto subpixel_row_from_reference_row = [offset_scaled_block_y](u32 row) {
        return (offset_scaled_block_y >> SUBPEL_BITS) + static_cast<i32>(row);
    };
    auto reference_index_for_row = [reference_frame_width](i32 row) {
        return static_cast<size_t>(MV_BORDER + row) * reference_frame_width;
    };

    // The variable intermediateHeight specifying the height required for the intermediate array is set equal to (((h -
    // 1) * yStep + 15) >> 4) + 8.
    static constexpr auto maximum_intermediate_height = (((maximum_block_dimensions - 1) * maximum_scaled_step + 15) >> 4) + 8;
    auto const intermediate_height = (((height - 1) * scaled_step_y + 15) >> 4) + 8;
    VERIFY(intermediate_height <= maximum_intermediate_height);
    // Check our reference frame bounds before starting the loop.
    auto const last_possible_reference_index = reference_index_for_row(subpixel_row_from_reference_row(intermediate_height - sample_offset));
    VERIFY(reference_frame_buffer.size() >= last_possible_reference_index);

    VERIFY(block_buffer.size() >= static_cast<size_t>(width) * height);

    auto const reference_block_x = MV_BORDER + (offset_scaled_block_x >> SUBPEL_BITS);
    auto const reference_block_y = MV_BORDER + (offset_scaled_block_y >> SUBPEL_BITS);
    auto const reference_subpixel_x = offset_scaled_block_x & SUBPEL_MASK;
    auto const reference_subpixel_y = offset_scaled_block_y & SUBPEL_MASK;

    // OPTIMIZATION: If the fractional part of a component of the motion vector is 0, we want to do a fast path
    //               skipping one or both of the convolutions.
    bool const copy_x = reference_subpixel_x == 0;
    bool const copy_y = reference_subpixel_y == 0;
    bool const unscaled_x = scaled_step_x == 16;
    bool const unscaled_y = scaled_step_y == 16;

    // The array intermediate is specified as follows:
    // Note: Height is specified by `intermediate_height`, width is specified by `width`
    Array<u16, maximum_intermediate_height * maximum_block_dimensions> intermediate_buffer;
    auto const bit_depth = block_context.frame_context.color_config.bit_depth;
    auto const* reference_start = reference_frame_buffer.data() + reference_block_y * reference_frame_width + reference_block_x;

    // FIXME: We are using 16-bit products to vectorize the filter loops, but when filtering in a high bit-depth video, they will truncate.
    //        Instead of hardcoding them, we should have the bit depth as a template parameter, and the accumulators can select a size based
    //        on whether the bit depth > 8.
    //        Note that we only get a benefit from this on the default CPU target. If we enable AVX2 here, we may want to specialize the
    //        function for the CPU target and remove the cast to i16 so that it doesn't have to truncate on AVX2, where it can do the full
    //        unrolled 32-bit product loops in one vector.

    if (unscaled_x && unscaled_y && bit_depth == 8) {
        if (copy_x && copy_y) {
            // We can memcpy here to avoid doing any real work.
            auto const* reference_scan_line = &reference_frame_buffer[reference_block_y * reference_frame_width + reference_block_x];
            auto* destination_scan_line = block_buffer.data();

            for (auto row = 0u; row < height; row++) {
                memcpy(destination_scan_line, reference_scan_line, width * sizeof(*destination_scan_line));
                reference_scan_line += reference_frame_width;
                destination_scan_line += width;
            }

            return {};
        }

        auto horizontal_convolution_unscaled = [](auto bit_depth, auto* destination, auto width, auto height, auto const* source, auto source_stride, auto filter, auto subpixel_x) {
            source -= sample_offset;
            auto const source_end_skip = source_stride - width;

            for (auto row = 0u; row < height; row++) {
                for (auto column = 0u; column < width; column++) {
                    i32 accumulated_samples = 0;
                    for (auto t = 0; t < 8; t++) {
                        auto sample = source[t];
                        accumulated_samples += static_cast<i16>(subpel_filters[filter][subpixel_x][t] * sample);
                    }

                    *destination = clip_1(bit_depth, rounded_right_shift(accumulated_samples, 7));
                    source++;
                    destination++;
                }
                source += source_end_skip;
            }
        };

        if (copy_y) {
            horizontal_convolution_unscaled(bit_depth, block_buffer.data(), width, height, reference_start, reference_frame_width, block_context.interpolation_filter, reference_subpixel_x);
            return {};
        }

        auto vertical_convolution_unscaled = [](auto bit_depth, auto* destination, auto width, auto height, auto const* source, auto source_stride, auto filter, auto subpixel_y) {
            auto const source_end_skip = source_stride - width;

            for (auto row = 0u; row < height; row++) {
                for (auto column = 0u; column < width; column++) {
                    auto const* scan_column = source;
                    i32 accumulated_samples = 0;
                    for (auto t = 0; t < 8; t++) {
                        auto sample = *scan_column;
                        accumulated_samples += static_cast<i16>(subpel_filters[filter][subpixel_y][t] * sample);
                        scan_column += source_stride;
                    }
                    *destination = clip_1(bit_depth, rounded_right_shift(accumulated_samples, 7));
                    source++;
                    destination++;
                }
                source += source_end_skip;
            }
        };

        if (copy_x) {
            vertical_convolution_unscaled(bit_depth, block_buffer.data(), width, height, reference_start - (sample_offset * reference_frame_width), reference_frame_width, block_context.interpolation_filter, reference_subpixel_y);
            return {};
        }

        horizontal_convolution_unscaled(bit_depth, intermediate_buffer.data(), width, intermediate_height, reference_start - (sample_offset * reference_frame_width), reference_frame_width, block_context.interpolation_filter, reference_subpixel_x);
        vertical_convolution_unscaled(bit_depth, block_buffer.data(), width, height, intermediate_buffer.data(), width, block_context.interpolation_filter, reference_subpixel_y);
        return {};
    }

    // NOTE: Accumulators below are 32-bit to allow high bit-depth videos to decode without overflows.
    //       These should be changed when the accumulators above are.

    auto horizontal_convolution_scaled = [](auto bit_depth, auto* destination, auto width, auto height, auto const* source, auto source_stride, auto filter, auto subpixel_x, auto scale_x) {
        source -= sample_offset;

        for (auto row = 0u; row < height; row++) {
            auto scan_subpixel = subpixel_x;
            for (auto column = 0u; column < width; column++) {
                auto const* scan_line = source + (scan_subpixel >> 4);
                i32 accumulated_samples = 0;
                for (auto t = 0; t < 8; t++) {
                    auto sample = scan_line[t];
                    accumulated_samples += subpel_filters[filter][scan_subpixel & SUBPEL_MASK][t] * sample;
                }

                *destination = clip_1(bit_depth, rounded_right_shift(accumulated_samples, 7));
                destination++;
                scan_subpixel += scale_x;
            }
            source += source_stride;
        }
    };

    auto vertical_convolution_scaled = [](auto bit_depth, auto* destination, auto width, auto height, auto const* source, auto source_stride, auto filter, auto subpixel_y, auto scale_y) {
        for (auto row = 0u; row < height; row++) {
            auto const* source_column_base = source + (subpixel_y >> SUBPEL_BITS) * source_stride;

            for (auto column = 0u; column < width; column++) {
                auto const* scan_column = source_column_base + column;
                i32 accumulated_samples = 0;
                for (auto t = 0; t < 8; t++) {
                    auto sample = *scan_column;
                    accumulated_samples += subpel_filters[filter][subpixel_y & SUBPEL_MASK][t] * sample;
                    scan_column += source_stride;
                }

                *destination = clip_1(bit_depth, rounded_right_shift(accumulated_samples, 7));
                destination++;
            }
            subpixel_y += scale_y;
        }
    };

    horizontal_convolution_scaled(bit_depth, intermediate_buffer.data(), width, intermediate_height, reference_start - (sample_offset * reference_frame_width), reference_frame_width, block_context.interpolation_filter, offset_scaled_block_x & SUBPEL_MASK, scaled_step_x);
    vertical_convolution_scaled(bit_depth, block_buffer.data(), width, height, intermediate_buffer.data(), width, block_context.interpolation_filter, reference_subpixel_y, scaled_step_y);

    return {};
}

DecoderErrorOr<void> Decoder::predict_inter(u8 plane, BlockContext const& block_context, u32 x, u32 y, u32 width, u32 height, u32 block_index)
{
    // The inter prediction process is invoked for inter coded blocks. When MiSize is smaller than BLOCK_8X8, the
    // prediction is done with a granularity of 4x4 samples, otherwise the whole plane is predicted at the same time.
    // The inputs to this process are:
    // − a variable plane specifying which plane is being predicted,
    // − variables x and y specifying the location of the top left sample in the CurrFrame[ plane ] array of the region
    // to be predicted,
    // − variables w and h specifying the width and height of the region to be predicted,
    // − a variable blockIdx, specifying how much of the block has already been predicted in units of 4x4 samples.
    // The outputs of this process are inter predicted samples in the current frame CurrFrame.

    // The prediction arrays are formed by the following ordered steps:
    // 1. The variable refList is set equal to 0.
    // 2. through 5.
    Array<u16, maximum_block_size> predicted_buffer;
    auto predicted_span = predicted_buffer.span().trim(width * height);
    TRY(predict_inter_block(plane, block_context, ReferenceIndex::Primary, block_context.row, block_context.column, x, y, width, height, block_index, predicted_span));
    auto predicted_buffer_at = [&](Span<u16> buffer, u32 row, u32 column) -> u16& {
        return buffer[row * width + column];
    };

    // 6. If isCompound is equal to 1, then the variable refList is set equal to 1 and steps 2, 3, 4 and 5 are repeated
    // to form the prediction for the second reference.
    // The inter predicted samples are then derived as follows:
    auto& frame_buffer = get_output_buffer(plane);
    VERIFY(!frame_buffer.is_empty());
    auto frame_size = block_context.frame_context.decoded_size(plane > 0);
    auto frame_buffer_at = [&](u32 row, u32 column) -> u16& {
        return frame_buffer[row * frame_size.width() + column];
    };

    auto width_in_frame_buffer = min(width, frame_size.width() - x);
    auto height_in_frame_buffer = min(height, frame_size.height() - y);

    // The variable isCompound is set equal to ref_frame[ 1 ] > NONE.
    // − If isCompound is equal to 0, CurrFrame[ plane ][ y + i ][ x + j ] is set equal to preds[ 0 ][ i ][ j ] for i = 0..h-1
    // and j = 0..w-1.
    if (!block_context.is_compound()) {
        for (auto i = 0u; i < height_in_frame_buffer; i++) {
            for (auto j = 0u; j < width_in_frame_buffer; j++)
                frame_buffer_at(y + i, x + j) = predicted_buffer_at(predicted_span, i, j);
        }

        return {};
    }

    // − Otherwise, CurrFrame[ plane ][ y + i ][ x + j ] is set equal to Round2( preds[ 0 ][ i ][ j ] + preds[ 1 ][ i ][ j ], 1 )
    // for i = 0..h-1 and j = 0..w-1.
    Array<u16, maximum_block_size> second_predicted_buffer;
    auto second_predicted_span = second_predicted_buffer.span().trim(width * height);
    TRY(predict_inter_block(plane, block_context, ReferenceIndex::Secondary, block_context.row, block_context.column, x, y, width, height, block_index, second_predicted_span));

    for (auto i = 0u; i < height_in_frame_buffer; i++) {
        for (auto j = 0u; j < width_in_frame_buffer; j++)
            frame_buffer_at(y + i, x + j) = rounded_right_shift(predicted_buffer_at(predicted_span, i, j) + predicted_buffer_at(second_predicted_span, i, j), 1);
    }

    return {};
}

inline u16 dc_q(u8 bit_depth, u8 b)
{
    // The function dc_q( b ) is specified as dc_qlookup[ (BitDepth-8) >> 1 ][ Clip3( 0, 255, b ) ] where dc_lookup is
    // defined as follows:
    constexpr u16 dc_qlookup[3][256] = {
        { 4, 8, 8, 9, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 23, 24, 25, 26, 26, 27, 28, 29, 30, 31, 32, 32, 33, 34, 35, 36, 37, 38, 38, 39, 40, 41, 42, 43, 43, 44, 45, 46, 47, 48, 48, 49, 50, 51, 52, 53, 53, 54, 55, 56, 57, 57, 58, 59, 60, 61, 62, 62, 63, 64, 65, 66, 66, 67, 68, 69, 70, 70, 71, 72, 73, 74, 74, 75, 76, 77, 78, 78, 79, 80, 81, 81, 82, 83, 84, 85, 85, 87, 88, 90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 108, 110, 111, 113, 114, 116, 117, 118, 120, 121, 123, 125, 127, 129, 131, 134, 136, 138, 140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 161, 164, 166, 169, 172, 174, 177, 180, 182, 185, 187, 190, 192, 195, 199, 202, 205, 208, 211, 214, 217, 220, 223, 226, 230, 233, 237, 240, 243, 247, 250, 253, 257, 261, 265, 269, 272, 276, 280, 284, 288, 292, 296, 300, 304, 309, 313, 317, 322, 326, 330, 335, 340, 344, 349, 354, 359, 364, 369, 374, 379, 384, 389, 395, 400, 406, 411, 417, 423, 429, 435, 441, 447, 454, 461, 467, 475, 482, 489, 497, 505, 513, 522, 530, 539, 549, 559, 569, 579, 590, 602, 614, 626, 640, 654, 668, 684, 700, 717, 736, 755, 775, 796, 819, 843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336 },
        { 4, 9, 10, 13, 15, 17, 20, 22, 25, 28, 31, 34, 37, 40, 43, 47, 50, 53, 57, 60, 64, 68, 71, 75, 78, 82, 86, 90, 93, 97, 101, 105, 109, 113, 116, 120, 124, 128, 132, 136, 140, 143, 147, 151, 155, 159, 163, 166, 170, 174, 178, 182, 185, 189, 193, 197, 200, 204, 208, 212, 215, 219, 223, 226, 230, 233, 237, 241, 244, 248, 251, 255, 259, 262, 266, 269, 273, 276, 280, 283, 287, 290, 293, 297, 300, 304, 307, 310, 314, 317, 321, 324, 327, 331, 334, 337, 343, 350, 356, 362, 369, 375, 381, 387, 394, 400, 406, 412, 418, 424, 430, 436, 442, 448, 454, 460, 466, 472, 478, 484, 490, 499, 507, 516, 525, 533, 542, 550, 559, 567, 576, 584, 592, 601, 609, 617, 625, 634, 644, 655, 666, 676, 687, 698, 708, 718, 729, 739, 749, 759, 770, 782, 795, 807, 819, 831, 844, 856, 868, 880, 891, 906, 920, 933, 947, 961, 975, 988, 1001, 1015, 1030, 1045, 1061, 1076, 1090, 1105, 1120, 1137, 1153, 1170, 1186, 1202, 1218, 1236, 1253, 1271, 1288, 1306, 1323, 1342, 1361, 1379, 1398, 1416, 1436, 1456, 1476, 1496, 1516, 1537, 1559, 1580, 1601, 1624, 1647, 1670, 1692, 1717, 1741, 1766, 1791, 1817, 1844, 1871, 1900, 1929, 1958, 1990, 2021, 2054, 2088, 2123, 2159, 2197, 2236, 2276, 2319, 2363, 2410, 2458, 2508, 2561, 2616, 2675, 2737, 2802, 2871, 2944, 3020, 3102, 3188, 3280, 3375, 3478, 3586, 3702, 3823, 3953, 4089, 4236, 4394, 4559, 4737, 4929, 5130, 5347 },
        { 4, 12, 18, 25, 33, 41, 50, 60, 70, 80, 91, 103, 115, 127, 140, 153, 166, 180, 194, 208, 222, 237, 251, 266, 281, 296, 312, 327, 343, 358, 374, 390, 405, 421, 437, 453, 469, 484, 500, 516, 532, 548, 564, 580, 596, 611, 627, 643, 659, 674, 690, 706, 721, 737, 752, 768, 783, 798, 814, 829, 844, 859, 874, 889, 904, 919, 934, 949, 964, 978, 993, 1008, 1022, 1037, 1051, 1065, 1080, 1094, 1108, 1122, 1136, 1151, 1165, 1179, 1192, 1206, 1220, 1234, 1248, 1261, 1275, 1288, 1302, 1315, 1329, 1342, 1368, 1393, 1419, 1444, 1469, 1494, 1519, 1544, 1569, 1594, 1618, 1643, 1668, 1692, 1717, 1741, 1765, 1789, 1814, 1838, 1862, 1885, 1909, 1933, 1957, 1992, 2027, 2061, 2096, 2130, 2165, 2199, 2233, 2267, 2300, 2334, 2367, 2400, 2434, 2467, 2499, 2532, 2575, 2618, 2661, 2704, 2746, 2788, 2830, 2872, 2913, 2954, 2995, 3036, 3076, 3127, 3177, 3226, 3275, 3324, 3373, 3421, 3469, 3517, 3565, 3621, 3677, 3733, 3788, 3843, 3897, 3951, 4005, 4058, 4119, 4181, 4241, 4301, 4361, 4420, 4479, 4546, 4612, 4677, 4742, 4807, 4871, 4942, 5013, 5083, 5153, 5222, 5291, 5367, 5442, 5517, 5591, 5665, 5745, 5825, 5905, 5984, 6063, 6149, 6234, 6319, 6404, 6495, 6587, 6678, 6769, 6867, 6966, 7064, 7163, 7269, 7376, 7483, 7599, 7715, 7832, 7958, 8085, 8214, 8352, 8492, 8635, 8788, 8945, 9104, 9275, 9450, 9639, 9832, 10031, 10245, 10465, 10702, 10946, 11210, 11482, 11776, 12081, 12409, 12750, 13118, 13501, 13913, 14343, 14807, 15290, 15812, 16356, 16943, 17575, 18237, 18949, 19718, 20521, 21387 }
    };

    return dc_qlookup[(bit_depth - 8) >> 1][clip_3<u8>(0, 255, b)];
}

inline u16 ac_q(u8 bit_depth, u8 b)
{
    // The function ac_q( b ) is specified as ac_qlookup[ (BitDepth-8) >> 1 ][ Clip3( 0, 255, b ) ] where ac_lookup is
    // defined as follows:
    constexpr u16 ac_qlookup[3][256] = {
        { 4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 142, 144, 146, 148, 150, 152, 155, 158, 161, 164, 167, 170, 173, 176, 179, 182, 185, 188, 191, 194, 197, 200, 203, 207, 211, 215, 219, 223, 227, 231, 235, 239, 243, 247, 251, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300, 305, 311, 317, 323, 329, 335, 341, 347, 353, 359, 366, 373, 380, 387, 394, 401, 408, 416, 424, 432, 440, 448, 456, 465, 474, 483, 492, 501, 510, 520, 530, 540, 550, 560, 571, 582, 593, 604, 615, 627, 639, 651, 663, 676, 689, 702, 715, 729, 743, 757, 771, 786, 801, 816, 832, 848, 864, 881, 898, 915, 933, 951, 969, 988, 1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196, 1219, 1243, 1267, 1292, 1317, 1343, 1369, 1396, 1423, 1451, 1479, 1508, 1537, 1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828 },
        { 4, 9, 11, 13, 16, 18, 21, 24, 27, 30, 33, 37, 40, 44, 48, 51, 55, 59, 63, 67, 71, 75, 79, 83, 88, 92, 96, 100, 105, 109, 114, 118, 122, 127, 131, 136, 140, 145, 149, 154, 158, 163, 168, 172, 177, 181, 186, 190, 195, 199, 204, 208, 213, 217, 222, 226, 231, 235, 240, 244, 249, 253, 258, 262, 267, 271, 275, 280, 284, 289, 293, 297, 302, 306, 311, 315, 319, 324, 328, 332, 337, 341, 345, 349, 354, 358, 362, 367, 371, 375, 379, 384, 388, 392, 396, 401, 409, 417, 425, 433, 441, 449, 458, 466, 474, 482, 490, 498, 506, 514, 523, 531, 539, 547, 555, 563, 571, 579, 588, 596, 604, 616, 628, 640, 652, 664, 676, 688, 700, 713, 725, 737, 749, 761, 773, 785, 797, 809, 825, 841, 857, 873, 889, 905, 922, 938, 954, 970, 986, 1002, 1018, 1038, 1058, 1078, 1098, 1118, 1138, 1158, 1178, 1198, 1218, 1242, 1266, 1290, 1314, 1338, 1362, 1386, 1411, 1435, 1463, 1491, 1519, 1547, 1575, 1603, 1631, 1663, 1695, 1727, 1759, 1791, 1823, 1859, 1895, 1931, 1967, 2003, 2039, 2079, 2119, 2159, 2199, 2239, 2283, 2327, 2371, 2415, 2459, 2507, 2555, 2603, 2651, 2703, 2755, 2807, 2859, 2915, 2971, 3027, 3083, 3143, 3203, 3263, 3327, 3391, 3455, 3523, 3591, 3659, 3731, 3803, 3876, 3952, 4028, 4104, 4184, 4264, 4348, 4432, 4516, 4604, 4692, 4784, 4876, 4972, 5068, 5168, 5268, 5372, 5476, 5584, 5692, 5804, 5916, 6032, 6148, 6268, 6388, 6512, 6640, 6768, 6900, 7036, 7172, 7312 },
        { 4, 13, 19, 27, 35, 44, 54, 64, 75, 87, 99, 112, 126, 139, 154, 168, 183, 199, 214, 230, 247, 263, 280, 297, 314, 331, 349, 366, 384, 402, 420, 438, 456, 475, 493, 511, 530, 548, 567, 586, 604, 623, 642, 660, 679, 698, 716, 735, 753, 772, 791, 809, 828, 846, 865, 884, 902, 920, 939, 957, 976, 994, 1012, 1030, 1049, 1067, 1085, 1103, 1121, 1139, 1157, 1175, 1193, 1211, 1229, 1246, 1264, 1282, 1299, 1317, 1335, 1352, 1370, 1387, 1405, 1422, 1440, 1457, 1474, 1491, 1509, 1526, 1543, 1560, 1577, 1595, 1627, 1660, 1693, 1725, 1758, 1791, 1824, 1856, 1889, 1922, 1954, 1987, 2020, 2052, 2085, 2118, 2150, 2183, 2216, 2248, 2281, 2313, 2346, 2378, 2411, 2459, 2508, 2556, 2605, 2653, 2701, 2750, 2798, 2847, 2895, 2943, 2992, 3040, 3088, 3137, 3185, 3234, 3298, 3362, 3426, 3491, 3555, 3619, 3684, 3748, 3812, 3876, 3941, 4005, 4069, 4149, 4230, 4310, 4390, 4470, 4550, 4631, 4711, 4791, 4871, 4967, 5064, 5160, 5256, 5352, 5448, 5544, 5641, 5737, 5849, 5961, 6073, 6185, 6297, 6410, 6522, 6650, 6778, 6906, 7034, 7162, 7290, 7435, 7579, 7723, 7867, 8011, 8155, 8315, 8475, 8635, 8795, 8956, 9132, 9308, 9484, 9660, 9836, 10028, 10220, 10412, 10604, 10812, 11020, 11228, 11437, 11661, 11885, 12109, 12333, 12573, 12813, 13053, 13309, 13565, 13821, 14093, 14365, 14637, 14925, 15213, 15502, 15806, 16110, 16414, 16734, 17054, 17390, 17726, 18062, 18414, 18766, 19134, 19502, 19886, 20270, 20670, 21070, 21486, 21902, 22334, 22766, 23214, 23662, 24126, 24590, 25070, 25551, 26047, 26559, 27071, 27599, 28143, 28687, 29247 }
    };

    return ac_qlookup[(bit_depth - 8) >> 1][clip_3<u8>(0, 255, b)];
}

u8 Decoder::get_base_quantizer_index(SegmentFeatureStatus alternative_quantizer_feature, bool should_use_absolute_segment_base_quantizer, u8 base_quantizer_index)
{
    // The function get_qindex( ) returns the quantizer index for the current block and is specified by the following:
    // − If seg_feature_active( SEG_LVL_ALT_Q ) is equal to 1 the following ordered steps apply:
    if (alternative_quantizer_feature.enabled) {
        // 1. Set the variable data equal to FeatureData[ segment_id ][ SEG_LVL_ALT_Q ].
        auto data = alternative_quantizer_feature.value;

        // 2. If segmentation_abs_or_delta_update is equal to 0, set data equal to base_q_idx + data
        if (!should_use_absolute_segment_base_quantizer) {
            data += base_quantizer_index;
        }

        // 3. Return Clip3( 0, 255, data ).
        return clip_3<u8>(0, 255, data);
    }

    // − Otherwise, return base_q_idx.
    return base_quantizer_index;
}

u16 Decoder::get_dc_quantizer(u8 bit_depth, u8 base, i8 delta)
{
    // NOTE: Delta is selected by the caller based on whether it is for the Y or UV planes.

    // The function get_dc_quant( plane ) returns the quantizer value for the dc coefficient for a particular plane and
    // is derived as follows:
    // − If plane is equal to 0, return dc_q( get_qindex( ) + delta_q_y_dc ).
    // − Otherwise, return dc_q( get_qindex( ) + delta_q_uv_dc ).
    return dc_q(bit_depth, static_cast<u8>(base + delta));
}

u16 Decoder::get_ac_quantizer(u8 bit_depth, u8 base, i8 delta)
{
    // NOTE: Delta is selected by the caller based on whether it is for the Y or UV planes.

    // The function get_ac_quant( plane ) returns the quantizer value for the ac coefficient for a particular plane and
    // is derived as follows:
    // − If plane is equal to 0, return ac_q( get_qindex( ) ).
    // − Otherwise, return ac_q( get_qindex( ) + delta_q_uv_ac ).
    return ac_q(bit_depth, static_cast<u8>(base + delta));
}

DecoderErrorOr<void> Decoder::reconstruct(u8 plane, BlockContext const& block_context, u32 transform_block_x, u32 transform_block_y, TransformSize transform_block_size, TransformSet transform_set)
{
    // 8.6.2 Reconstruct process

    // The variable n (specifying the base 2 logarithm of the width of the transform block) is set equal to 2 + txSz.
    u8 log2_of_block_size = 2u + transform_block_size;
    switch (log2_of_block_size) {
    case 2:
        return reconstruct_templated<2>(plane, block_context, transform_block_x, transform_block_y, transform_set);
        break;
    case 3:
        return reconstruct_templated<3>(plane, block_context, transform_block_x, transform_block_y, transform_set);
        break;
    case 4:
        return reconstruct_templated<4>(plane, block_context, transform_block_x, transform_block_y, transform_set);
        break;
    case 5:
        return reconstruct_templated<5>(plane, block_context, transform_block_x, transform_block_y, transform_set);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

template<u8 log2_of_block_size>
DecoderErrorOr<void> Decoder::reconstruct_templated(u8 plane, BlockContext const& block_context, u32 transform_block_x, u32 transform_block_y, TransformSet transform_set)
{
    // 8.6.2 Reconstruct process, continued:

    // The variable dqDenom is set equal to 2 if txSz is equal to Transform_32X32, otherwise dqDenom is set equal to 1.
    constexpr Intermediate dq_denominator = log2_of_block_size == 5 ? 2 : 1;
    // The variable n0 (specifying the width of the transform block) is set equal to 1 << n.
    constexpr auto block_size = 1u << log2_of_block_size;

    // 1. Dequant[ i ][ j ] is set equal to ( Tokens[ i * n0 + j ] * get_ac_quant( plane ) ) / dqDenom
    //    for i = 0..(n0-1), for j = 0..(n0-1)
    Array<Intermediate, block_size * block_size> dequantized;
    auto quantizers = block_context.frame_context.segment_quantizers[block_context.segment_id];
    Intermediate ac_quant = plane == 0 ? quantizers.y_ac_quantizer : quantizers.uv_ac_quantizer;
    auto const* tokens_raw = block_context.residual_tokens.data();
    for (u32 i = 0; i < dequantized.size(); i++) {
        dequantized[i] = (tokens_raw[i] * ac_quant) / dq_denominator;
    }

    // 2. Dequant[ 0 ][ 0 ] is set equal to ( Tokens[ 0 ] * get_dc_quant( plane ) ) / dqDenom
    dequantized[0] = (block_context.residual_tokens[0] * (plane == 0 ? quantizers.y_dc_quantizer : quantizers.uv_dc_quantizer)) / dq_denominator;

    // It is a requirement of bitstream conformance that the values written into the Dequant array in steps 1 and 2
    // are representable by a signed integer with 8 + BitDepth bits.
    // Note: Since bounds checks just ensure that we will not have resulting values that will overflow, it's non-fatal
    // to allow these bounds to be violated. Therefore, we can avoid the performance cost here.

    // 3. Invoke the 2D inverse transform block process defined in section 8.7.2 with the variable n as input.
    //    The inverse transform outputs are stored back to the Dequant buffer.
    TRY(inverse_transform_2d<log2_of_block_size>(block_context, dequantized, transform_set));

    // 4. CurrFrame[ plane ][ y + i ][ x + j ] is set equal to Clip1( CurrFrame[ plane ][ y + i ][ x + j ] + Dequant[ i ][ j ] )
    //    for i = 0..(n0-1) and j = 0..(n0-1).
    auto& current_buffer = get_output_buffer(plane);
    auto frame_size = block_context.frame_context.decoded_size(plane > 0);
    auto width_in_frame_buffer = min(block_size, frame_size.width() - transform_block_x);
    auto height_in_frame_buffer = min(block_size, frame_size.height() - transform_block_y);

    for (auto i = 0u; i < height_in_frame_buffer; i++) {
        for (auto j = 0u; j < width_in_frame_buffer; j++) {
            auto index = (transform_block_y + i) * frame_size.width() + transform_block_x + j;
            auto dequantized_value = dequantized[i * block_size + j];
            current_buffer[index] = clip_1(block_context.frame_context.color_config.bit_depth, current_buffer[index] + dequantized_value);
        }
    }

    return {};
}

inline DecoderErrorOr<void> Decoder::inverse_walsh_hadamard_transform(Span<Intermediate> data, u8 log2_of_block_size, u8 shift)
{
    // The input to this process is a variable shift that specifies the amount of pre-scaling.
    // This process does an in-place transform of the array T (of length 4) by the following ordered steps:
    if (1 << log2_of_block_size != 4)
        return DecoderError::corrupted("Block size was not 4"sv);

    auto a = data[0] >> shift;
    auto c = data[1] >> shift;
    auto d = data[2] >> shift;
    auto b = data[3] >> shift;
    a += c;
    d -= b;
    auto average_of_a_and_d = (a - d) >> 1;
    b = average_of_a_and_d - b;
    c = average_of_a_and_d - c;
    a -= b;
    d += c;
    data[0] = a;
    data[1] = b;
    data[2] = c;
    data[3] = d;
    return {};
}

inline i32 Decoder::cos64(u8 angle)
{
    i32 const cos64_lookup[33] = { 16384, 16364, 16305, 16207, 16069, 15893, 15679, 15426, 15137, 14811, 14449, 14053, 13623, 13160, 12665, 12140, 11585, 11003, 10394, 9760, 9102, 8423, 7723, 7005, 6270, 5520, 4756, 3981, 3196, 2404, 1606, 804, 0 };

    // 1. Set a variable angle2 equal to angle & 127.
    angle &= 127;
    // 2. If angle2 is greater than or equal to 0 and less than or equal to 32, return cos64_lookup[ angle2 ].
    if (angle <= 32)
        return cos64_lookup[angle];
    // 3. If angle2 is greater than 32 and less than or equal to 64, return cos64_lookup[ 64 - angle2 ] * -1.
    if (angle <= 64)
        return -cos64_lookup[64 - angle];
    // 4. If angle2 is greater than 64 and less than or equal to 96, return cos64_lookup[ angle2 - 64 ] * -1.
    if (angle <= 96)
        return -cos64_lookup[angle - 64];
    // 5. Otherwise (if angle2 is greater than 96 and less than 128), return cos64_lookup[ 128 - angle2 ].
    return cos64_lookup[128 - angle];
}

inline i32 Decoder::sin64(u8 angle)
{
    if (angle < 32)
        angle += 128;
    return cos64(angle - 32u);
}

// (8.7.1.1) The function B( a, b, angle, 0 ) performs a butterfly rotation.
inline void Decoder::butterfly_rotation_in_place(Span<Intermediate> data, size_t index_a, size_t index_b, u8 angle, bool flip)
{
    auto cos = cos64(angle);
    auto sin = sin64(angle);
    // 1. The variable x is set equal to T[ a ] * cos64( angle ) - T[ b ] * sin64( angle ).
    i64 rotated_a = static_cast<i64>(data[index_a]) * cos - static_cast<i64>(data[index_b]) * sin;
    // 2. The variable y is set equal to T[ a ] * sin64( angle ) + T[ b ] * cos64( angle ).
    i64 rotated_b = static_cast<i64>(data[index_a]) * sin + static_cast<i64>(data[index_b]) * cos;
    // 3. T[ a ] is set equal to Round2( x, 14 ).
    data[index_a] = rounded_right_shift(rotated_a, 14);
    // 4. T[ b ] is set equal to Round2( y, 14 ).
    data[index_b] = rounded_right_shift(rotated_b, 14);

    // The function B( a ,b, angle, 1 ) performs a butterfly rotation and flip specified by the following ordered steps:
    // 1. The function B( a, b, angle, 0 ) is invoked.
    // 2. The contents of T[ a ] and T[ b ] are exchanged.
    if (flip)
        swap(data[index_a], data[index_b]);

    // It is a requirement of bitstream conformance that the values saved into the array T by this function are
    // representable by a signed integer using 8 + BitDepth bits of precision.
    // Note: Since bounds checks just ensure that we will not have resulting values that will overflow, it's non-fatal
    // to allow these bounds to be violated. Therefore, we can avoid the performance cost here.
}

// (8.7.1.1) The function H( a, b, 0 ) performs a Hadamard rotation.
inline void Decoder::hadamard_rotation_in_place(Span<Intermediate> data, size_t index_a, size_t index_b, bool flip)
{
    // The function H( a, b, 1 ) performs a Hadamard rotation with flipped indices and is specified as follows:
    // 1. The function H( b, a, 0 ) is invoked.
    if (flip)
        swap(index_a, index_b);

    // The function H( a, b, 0 ) performs a Hadamard rotation specified by the following ordered steps:

    // 1. The variable x is set equal to T[ a ].
    auto a_value = data[index_a];
    // 2. The variable y is set equal to T[ b ].
    auto b_value = data[index_b];
    // 3. T[ a ] is set equal to x + y.
    data[index_a] = a_value + b_value;
    // 4. T[ b ] is set equal to x - y.
    data[index_b] = a_value - b_value;

    // It is a requirement of bitstream conformance that the values saved into the array T by this function are
    // representable by a signed integer using 8 + BitDepth bits of precision.
    // Note: Since bounds checks just ensure that we will not have resulting values that will overflow, it's non-fatal
    // to allow these bounds to be violated. Therefore, we can avoid the performance cost here.
}

template<u8 log2_of_block_size>
inline DecoderErrorOr<void> Decoder::inverse_discrete_cosine_transform_array_permutation(Span<Intermediate> data)
{
    static_assert(log2_of_block_size >= 2 && log2_of_block_size <= 5, "Block size out of range.");

    constexpr u8 block_size = 1 << log2_of_block_size;

    // This process performs an in-place permutation of the array T of length 2^n for 2 ≤ n ≤ 5 which is required before
    // execution of the inverse DCT process.
    if (log2_of_block_size < 2 || log2_of_block_size > 5)
        return DecoderError::corrupted("Block size was out of range"sv);

    // 1.1. A temporary array named copyT is set equal to T.
    Array<Intermediate, block_size> data_copy;
    AK::TypedTransfer<Intermediate>::copy(data_copy.data(), data.data(), block_size);

    // 1.2. T[ i ] is set equal to copyT[ brev( n, i ) ] for i = 0..((1<<n) - 1).
    for (auto i = 0u; i < block_size; i++)
        data[i] = data_copy[brev<log2_of_block_size>(i)];

    return {};
}

template<u8 log2_of_block_size>
ALWAYS_INLINE DecoderErrorOr<void> Decoder::inverse_discrete_cosine_transform(Span<Intermediate> data)
{
    static_assert(log2_of_block_size >= 2 && log2_of_block_size <= 5, "Block size out of range.");

    // 2.1. The variable n0 is set equal to 1<<n.
    constexpr u8 block_size = 1 << log2_of_block_size;

    // 8.7.1.3 Inverse DCT process

    // 2.2. The variable n1 is set equal to 1<<(n-1).
    constexpr u8 half_block_size = block_size >> 1;
    // 2.3 The variable n2 is set equal to 1<<(n-2).
    constexpr u8 quarter_block_size = half_block_size >> 1;
    // 2.4 The variable n3 is set equal to 1<<(n-3).
    constexpr u8 eighth_block_size = quarter_block_size >> 1;

    // 2.5 If n is equal to 2, invoke B( 0, 1, 16, 1 ), otherwise recursively invoke the inverse DCT defined in this
    // section with the variable n set equal to n - 1.
    if constexpr (log2_of_block_size == 2)
        butterfly_rotation_in_place(data, 0, 1, 16, true);
    else
        TRY(inverse_discrete_cosine_transform<log2_of_block_size - 1>(data));

    // 2.6 Invoke B( n1+i, n0-1-i, 32-brev( 5, n1+i), 0 ) for i = 0..(n2-1).
    for (auto i = 0u; i < quarter_block_size; i++) {
        auto index = half_block_size + i;
        butterfly_rotation_in_place(data, index, block_size - 1 - i, 32 - brev<5>(index), false);
    }

    // 2.7 If n is greater than or equal to 3:
    if constexpr (log2_of_block_size >= 3) {
        // a. Invoke H( n1+4*i+2*j, n1+1+4*i+2*j, j ) for i = 0..(n3-1), j = 0..1.
        for (auto i = 0u; i < eighth_block_size; i++) {
            for (auto j = 0u; j < 2; j++) {
                auto index = half_block_size + (4 * i) + (2 * j);
                hadamard_rotation_in_place(data, index, index + 1, j);
            }
        }
    }

    // 4. If n is equal to 5:
    if constexpr (log2_of_block_size == 5) {
        // a. Invoke B( n0-n+3-n2*j-4*i, n1+n-4+n2*j+4*i, 28-16*i+56*j, 1 ) for i = 0..1, j = 0..1.
        for (auto i = 0u; i < 2; i++) {
            for (auto j = 0u; j < 2; j++) {
                auto index_a = block_size - log2_of_block_size + 3 - (quarter_block_size * j) - (4 * i);
                auto index_b = half_block_size + log2_of_block_size - 4 + (quarter_block_size * j) + (4 * i);
                auto angle = 28 - (16 * i) + (56 * j);
                butterfly_rotation_in_place(data, index_a, index_b, angle, true);
            }
        }

        // b. Invoke H( n1+n3*j+i, n1+n2-5+n3*j-i, j&1 ) for i = 0..1, j = 0..3.
        for (auto i = 0u; i < 2; i++) {
            for (auto j = 0u; j < 4; j++) {
                auto index_a = half_block_size + (eighth_block_size * j) + i;
                auto index_b = half_block_size + quarter_block_size - 5 + (eighth_block_size * j) - i;
                hadamard_rotation_in_place(data, index_a, index_b, (j & 1) != 0);
            }
        }
    }

    // 5. If n is greater than or equal to 4:
    if constexpr (log2_of_block_size >= 4) {
        // a. Invoke B( n0-n+2-i-n2*j, n1+n-3+i+n2*j, 24+48*j, 1 ) for i = 0..(n==5), j = 0..1.
        for (auto i = 0u; i <= (log2_of_block_size == 5); i++) {
            for (auto j = 0u; j < 2; j++) {
                auto index_a = block_size - log2_of_block_size + 2 - i - (quarter_block_size * j);
                auto index_b = half_block_size + log2_of_block_size - 3 + i + (quarter_block_size * j);
                butterfly_rotation_in_place(data, index_a, index_b, 24 + (48 * j), true);
            }
        }

        // b. Invoke H( n1+n2*j+i, n1+n2-1+n2*j-i, j&1 ) for i = 0..(2n-7), j = 0..1.
        for (auto i = 0u; i < (2 * log2_of_block_size) - 6u; i++) {
            for (auto j = 0u; j < 2; j++) {
                auto index_a = half_block_size + (quarter_block_size * j) + i;
                auto index_b = half_block_size + quarter_block_size - 1 + (quarter_block_size * j) - i;
                hadamard_rotation_in_place(data, index_a, index_b, (j & 1) != 0);
            }
        }
    }

    // 6. If n is greater than or equal to 3:
    if constexpr (log2_of_block_size >= 3) {
        // a. Invoke B( n0-n3-1-i, n1+n3+i, 16, 1 ) for i = 0..(n3-1).
        for (auto i = 0u; i < eighth_block_size; i++) {
            auto index_a = block_size - eighth_block_size - 1 - i;
            auto index_b = half_block_size + eighth_block_size + i;
            butterfly_rotation_in_place(data, index_a, index_b, 16, true);
        }
    }

    // 7. Invoke H( i, n0-1-i, 0 ) for i = 0..(n1-1).
    for (auto i = 0u; i < half_block_size; i++)
        hadamard_rotation_in_place(data, i, block_size - 1 - i, false);

    return {};
}

template<u8 log2_of_block_size>
inline void Decoder::inverse_asymmetric_discrete_sine_transform_input_array_permutation(Span<Intermediate> data)
{
    // The variable n0 is set equal to 1<<n.
    constexpr auto block_size = 1u << log2_of_block_size;
    // The variable n1 is set equal to 1<<(n-1).
    // We can iterate by 2 at a time instead of taking half block size.

    // A temporary array named copyT is set equal to T.
    Array<Intermediate, block_size> data_copy;
    AK::TypedTransfer<Intermediate>::copy(data_copy.data(), data.data(), block_size);

    // The values at even locations T[ 2 * i ] are set equal to copyT[ n0 - 1 - 2 * i ] for i = 0..(n1-1).
    // The values at odd locations T[ 2 * i + 1 ] are set equal to copyT[ 2 * i ] for i = 0..(n1-1).
    for (auto i = 0u; i < block_size; i += 2) {
        data[i] = data_copy[block_size - 1 - i];
        data[i + 1] = data_copy[i];
    }
}

template<u8 log2_of_block_size>
inline void Decoder::inverse_asymmetric_discrete_sine_transform_output_array_permutation(Span<Intermediate> data)
{
    auto block_size = 1u << log2_of_block_size;

    // A temporary array named copyT is set equal to T.
    Array<Intermediate, maximum_transform_size> data_copy;
    AK::TypedTransfer<Intermediate>::copy(data_copy.data(), data.data(), block_size);

    // The permutation depends on n as follows:
    if (log2_of_block_size == 4) {
        // − If n is equal to 4,
        // T[ 8*a + 4*b + 2*c + d ] is set equal to copyT[ 8*(d^c) + 4*(c^b) + 2*(b^a) + a ] for a = 0..1
        // and b = 0..1 and c = 0..1 and d = 0..1.
        for (auto a = 0u; a < 2; a++)
            for (auto b = 0u; b < 2; b++)
                for (auto c = 0u; c < 2; c++)
                    for (auto d = 0u; d < 2; d++)
                        data[(8 * a) + (4 * b) + (2 * c) + d] = data_copy[8 * (d ^ c) + 4 * (c ^ b) + 2 * (b ^ a) + a];
    } else {
        VERIFY(log2_of_block_size == 3);
        // − Otherwise (n is equal to 3),
        // T[ 4*a + 2*b + c ] is set equal to copyT[ 4*(c^b) + 2*(b^a) + a ] for a = 0..1 and
        // b = 0..1 and c = 0..1.
        for (auto a = 0u; a < 2; a++)
            for (auto b = 0u; b < 2; b++)
                for (auto c = 0u; c < 2; c++)
                    data[4 * a + 2 * b + c] = data_copy[4 * (c ^ b) + 2 * (b ^ a) + a];
    }
}

inline void Decoder::inverse_asymmetric_discrete_sine_transform_4(Span<Intermediate> data)
{
    VERIFY(data.size() == 4);
    i64 const sinpi_1_9 = 5283;
    i64 const sinpi_2_9 = 9929;
    i64 const sinpi_3_9 = 13377;
    i64 const sinpi_4_9 = 15212;

    // Steps are derived from pseudocode in (8.7.1.6):
    // s0 = SINPI_1_9 * T[ 0 ]
    i64 s0 = sinpi_1_9 * data[0];
    // s1 = SINPI_2_9 * T[ 0 ]
    i64 s1 = sinpi_2_9 * data[0];
    // s2 = SINPI_3_9 * T[ 1 ]
    i64 s2 = sinpi_3_9 * data[1];
    // s3 = SINPI_4_9 * T[ 2 ]
    i64 s3 = sinpi_4_9 * data[2];
    // s4 = SINPI_1_9 * T[ 2 ]
    i64 s4 = sinpi_1_9 * data[2];
    // s5 = SINPI_2_9 * T[ 3 ]
    i64 s5 = sinpi_2_9 * data[3];
    // s6 = SINPI_4_9 * T[ 3 ]
    i64 s6 = sinpi_4_9 * data[3];
    // v = T[ 0 ] - T[ 2 ] + T[ 3 ]
    // s7 = SINPI_3_9 * v
    i64 s7 = sinpi_3_9 * (data[0] - data[2] + data[3]);

    // x0 = s0 + s3 + s5
    auto x0 = s0 + s3 + s5;
    // x1 = s1 - s4 - s6
    auto x1 = s1 - s4 - s6;
    // x2 = s7
    auto x2 = s7;
    // x3 = s2
    auto x3 = s2;

    // s0 = x0 + x3
    s0 = x0 + x3;
    // s1 = x1 + x3
    s1 = x1 + x3;
    // s2 = x2
    s2 = x2;
    // s3 = x0 + x1 - x3
    s3 = x0 + x1 - x3;

    // T[ 0 ] = Round2( s0, 14 )
    data[0] = rounded_right_shift(s0, 14);
    // T[ 1 ] = Round2( s1, 14 )
    data[1] = rounded_right_shift(s1, 14);
    // T[ 2 ] = Round2( s2, 14 )
    data[2] = rounded_right_shift(s2, 14);
    // T[ 3 ] = Round2( s3, 14 )
    data[3] = rounded_right_shift(s3, 14);

    // (8.7.1.1) The inverse asymmetric discrete sine transforms also make use of an intermediate array named S.
    // The values in this array require higher precision to avoid overflow. Using signed integers with 24 +
    // BitDepth bits of precision is enough to avoid overflow.
    // Note: Since bounds checks just ensure that we will not have resulting values that will overflow, it's non-fatal
    // to allow these bounds to be violated. Therefore, we can avoid the performance cost here.
}

// The function SB( a, b, angle, 0 ) performs a butterfly rotation.
// Spec defines the source as array T, and the destination array as S.
template<typename S, typename D>
inline void Decoder::butterfly_rotation(Span<S> source, Span<D> destination, size_t index_a, size_t index_b, u8 angle, bool flip)
{
    // The function SB( a, b, angle, 0 ) performs a butterfly rotation according to the following ordered steps:
    auto cos = cos64(angle);
    auto sin = sin64(angle);
    // Expand to the destination buffer's precision.
    D a = source[index_a];
    D b = source[index_b];
    // 1. S[ a ] is set equal to T[ a ] * cos64( angle ) - T[ b ] * sin64( angle ).
    destination[index_a] = a * cos - b * sin;
    // 2. S[ b ] is set equal to T[ a ] * sin64( angle ) + T[ b ] * cos64( angle ).
    destination[index_b] = a * sin + b * cos;

    // The function SB( a, b, angle, 1 ) performs a butterfly rotation and flip according to the following ordered steps:
    // 1. The function SB( a, b, angle, 0 ) is invoked.
    // 2. The contents of S[ a ] and S[ b ] are exchanged.
    if (flip)
        swap(destination[index_a], destination[index_b]);
}

// The function SH( a, b ) performs a Hadamard rotation and rounding.
// Spec defines the source array as S, and the destination array as T.
template<typename S, typename D>
inline void Decoder::hadamard_rotation(Span<S> source, Span<D> destination, size_t index_a, size_t index_b)
{
    // Keep the source buffer's precision until rounding.
    S a = source[index_a];
    S b = source[index_b];
    // 1. T[ a ] is set equal to Round2( S[ a ] + S[ b ], 14 ).
    destination[index_a] = rounded_right_shift(a + b, 14);
    // 2. T[ b ] is set equal to Round2( S[ a ] - S[ b ], 14 ).
    destination[index_b] = rounded_right_shift(a - b, 14);
}

inline DecoderErrorOr<void> Decoder::inverse_asymmetric_discrete_sine_transform_8(Span<Intermediate> data)
{
    VERIFY(data.size() == 8);
    // This process does an in-place transform of the array T using:

    // A higher precision array S for intermediate results.
    // (8.7.1.1) NOTE - The values in array S require higher precision to avoid overflow. Using signed integers with
    // 24 + BitDepth bits of precision is enough to avoid overflow.
    Array<i64, 8> high_precision_temp;

    // The following ordered steps apply:

    // 1. Invoke the ADST input array permutation process specified in section 8.7.1.4 with the input variable n set
    //    equal to 3.
    inverse_asymmetric_discrete_sine_transform_input_array_permutation<3>(data);

    // 2. Invoke SB( 2*i, 1+2*i, 30-8*i, 1 ) for i = 0..3.
    for (auto i = 0u; i < 4; i++)
        butterfly_rotation(data, high_precision_temp.span(), 2 * i, 1 + (2 * i), 30 - (8 * i), true);

    // 3. Invoke SH( i, 4+i ) for i = 0..3.
    for (auto i = 0u; i < 4; i++)
        hadamard_rotation(high_precision_temp.span(), data, i, 4 + i);

    // 4. Invoke SB( 4+3*i, 5+i, 24-16*i, 1 ) for i = 0..1.
    for (auto i = 0u; i < 2; i++)
        butterfly_rotation(data, high_precision_temp.span(), 4 + (3 * i), 5 + i, 24 - (16 * i), true);
    // 5. Invoke SH( 4+i, 6+i ) for i = 0..1.
    for (auto i = 0u; i < 2; i++)
        hadamard_rotation(high_precision_temp.span(), data, 4 + i, 6 + i);

    // 6. Invoke H( i, 2+i, 0 ) for i = 0..1.
    for (auto i = 0u; i < 2; i++)
        hadamard_rotation_in_place(data, i, 2 + i, false);

    // 7. Invoke B( 2+4*i, 3+4*i, 16, 1 ) for i = 0..1.
    for (auto i = 0u; i < 2; i++)
        butterfly_rotation_in_place(data, 2 + (4 * i), 3 + (4 * i), 16, true);

    // 8. Invoke the ADST output array permutation process specified in section 8.7.1.5 with the input variable n
    //    set equal to 3.
    inverse_asymmetric_discrete_sine_transform_output_array_permutation<3>(data);

    // 9. Set T[ 1+2*i ] equal to -T[ 1+2*i ] for i = 0..3.
    for (auto i = 0u; i < 4; i++) {
        auto index = 1 + (2 * i);
        data[index] = -data[index];
    }
    return {};
}

inline DecoderErrorOr<void> Decoder::inverse_asymmetric_discrete_sine_transform_16(Span<Intermediate> data)
{
    VERIFY(data.size() == 16);
    // This process does an in-place transform of the array T using:

    // A higher precision array S for intermediate results.
    // (8.7.1.1) The inverse asymmetric discrete sine transforms also make use of an intermediate array named S.
    // The values in this array require higher precision to avoid overflow. Using signed integers with 24 +
    // BitDepth bits of precision is enough to avoid overflow.
    Array<i64, 16> high_precision_temp;

    // The following ordered steps apply:

    // 1. Invoke the ADST input array permutation process specified in section 8.7.1.4 with the input variable n set
    // equal to 4.
    inverse_asymmetric_discrete_sine_transform_input_array_permutation<4>(data);

    // 2. Invoke SB( 2*i, 1+2*i, 31-4*i, 1 ) for i = 0..7.
    for (auto i = 0u; i < 8; i++)
        butterfly_rotation(data, high_precision_temp.span(), 2 * i, 1 + (2 * i), 31 - (4 * i), true);
    // 3. Invoke SH( i, 8+i ) for i = 0..7.
    for (auto i = 0u; i < 8; i++)
        hadamard_rotation(high_precision_temp.span(), data, i, 8 + i);

    // 4. Invoke SB( 8+2*i, 9+2*i, 28-16*i, 1 ) for i = 0..3.
    for (auto i = 0u; i < 4; i++)
        butterfly_rotation(data, high_precision_temp.span(), 8 + (2 * i), 9 + (2 * i), 128 + 28 - (16 * i), true);
    // 5. Invoke SH( 8+i, 12+i ) for i = 0..3.
    for (auto i = 0u; i < 4; i++)
        hadamard_rotation(high_precision_temp.span(), data, 8 + i, 12 + i);

    // 6. Invoke H( i, 4+i, 0 ) for i = 0..3.
    for (auto i = 0u; i < 4; i++)
        hadamard_rotation_in_place(data, i, 4 + i, false);

    // 7. Invoke SB( 4+8*i+3*j, 5+8*i+j, 24-16*j, 1 ) for i = 0..1, for j = 0..1.
    for (auto i = 0u; i < 2; i++)
        for (auto j = 0u; j < 2; j++)
            butterfly_rotation(data, high_precision_temp.span(), 4 + (8 * i) + (3 * j), 5 + (8 * i) + j, 24 - (16 * j), true);
    // 8. Invoke SH( 4+8*j+i, 6+8*j+i ) for i = 0..1, j = 0..1.
    for (auto i = 0u; i < 2; i++)
        for (auto j = 0u; j < 2; j++)
            hadamard_rotation(high_precision_temp.span(), data, 4 + (8 * j) + i, 6 + (8 * j) + i);

    // 9. Invoke H( 8*j+i, 2+8*j+i, 0 ) for i = 0..1, for j = 0..1.
    for (auto i = 0u; i < 2; i++)
        for (auto j = 0u; j < 2; j++)
            hadamard_rotation_in_place(data, (8 * j) + i, 2 + (8 * j) + i, false);
    // 10. Invoke B( 2+4*j+8*i, 3+4*j+8*i, 48+64*(i^j), 0 ) for i = 0..1, for j = 0..1.
    for (auto i = 0u; i < 2; i++)
        for (auto j = 0u; j < 2; j++)
            butterfly_rotation_in_place(data, 2 + (4 * j) + (8 * i), 3 + (4 * j) + (8 * i), 48 + (64 * (i ^ j)), false);

    // 11. Invoke the ADST output array permutation process specified in section 8.7.1.5 with the input variable n
    // set equal to 4.
    inverse_asymmetric_discrete_sine_transform_output_array_permutation<4>(data);

    // 12. Set T[ 1+12*j+2*i ] equal to -T[ 1+12*j+2*i ] for i = 0..1, for j = 0..1.
    for (auto i = 0u; i < 2; i++) {
        for (auto j = 0u; j < 2; j++) {
            auto index = 1 + (12 * j) + (2 * i);
            data[index] = -data[index];
        }
    }
    return {};
}

template<u8 log2_of_block_size>
inline DecoderErrorOr<void> Decoder::inverse_asymmetric_discrete_sine_transform(Span<Intermediate> data)
{
    // 8.7.1.9 Inverse ADST Process

    // This process performs an in-place inverse ADST process on the array T of size 2^n for 2 ≤ n ≤ 4.
    if constexpr (log2_of_block_size < 2 || log2_of_block_size > 4)
        return DecoderError::corrupted("Block size was out of range"sv);

    // The process to invoke depends on n as follows:
    if constexpr (log2_of_block_size == 2) {
        // − If n is equal to 2, invoke the Inverse ADST4 process specified in section 8.7.1.6.
        inverse_asymmetric_discrete_sine_transform_4(data);
        return {};
    }
    if constexpr (log2_of_block_size == 3) {
        // − Otherwise if n is equal to 3, invoke the Inverse ADST8 process specified in section 8.7.1.7.
        return inverse_asymmetric_discrete_sine_transform_8(data);
    }
    // − Otherwise (n is equal to 4), invoke the Inverse ADST16 process specified in section 8.7.1.8.
    return inverse_asymmetric_discrete_sine_transform_16(data);
}

template<u8 log2_of_block_size>
ALWAYS_INLINE DecoderErrorOr<void> Decoder::inverse_transform_2d(BlockContext const& block_context, Span<Intermediate> dequantized, TransformSet transform_set)
{
    static_assert(log2_of_block_size >= 2 && log2_of_block_size <= 5);

    // This process performs a 2D inverse transform for an array of size 2^n by 2^n stored in the 2D array Dequant.
    // The input to this process is a variable n (log2_of_block_size) that specifies the base 2 logarithm of the width of the transform.

    // 1. Set the variable n0 (block_size) equal to 1 << n.
    constexpr auto block_size = 1u << log2_of_block_size;

    Array<Intermediate, block_size * block_size> row_array;
    Span<Intermediate> row = row_array.span().trim(block_size);

    // 2. The row transforms with i = 0..(n0-1) are applied as follows:
    for (auto i = 0u; i < block_size; i++) {
        // 1. Set T[ j ] equal to Dequant[ i ][ j ] for j = 0..(n0-1).
        for (auto j = 0u; j < block_size; j++)
            row[j] = dequantized[i * block_size + j];

        // 2. If Lossless is equal to 1, invoke the Inverse WHT process as specified in section 8.7.1.10 with shift equal
        //    to 2.
        if (block_context.frame_context.lossless) {
            TRY(inverse_walsh_hadamard_transform(row, log2_of_block_size, 2));
        } else {
            switch (transform_set.second_transform) {
            case TransformType::DCT:
                // Otherwise, if TxType is equal to DCT_DCT or TxType is equal to ADST_DCT, apply an inverse DCT as
                // follows:
                // 1. Invoke the inverse DCT permutation process as specified in section 8.7.1.2 with the input variable n.
                TRY(inverse_discrete_cosine_transform_array_permutation<log2_of_block_size>(row));
                // 2. Invoke the inverse DCT process as specified in section 8.7.1.3 with the input variable n.
                TRY(inverse_discrete_cosine_transform<log2_of_block_size>(row));
                break;
            case TransformType::ADST:
                // 4. Otherwise (TxType is equal to DCT_ADST or TxType is equal to ADST_ADST), invoke the inverse ADST
                //    process as specified in section 8.7.1.9 with input variable n.
                TRY(inverse_asymmetric_discrete_sine_transform<log2_of_block_size>(row));
                break;
            default:
                return DecoderError::corrupted("Unknown tx_type"sv);
            }
        }

        // 5. Set Dequant[ i ][ j ] equal to T[ j ] for j = 0..(n0-1).
        for (auto j = 0u; j < block_size; j++)
            dequantized[i * block_size + j] = row[j];
    }

    Array<Intermediate, block_size * block_size> column_array;
    auto column = column_array.span().trim(block_size);

    // 3. The column transforms with j = 0..(n0-1) are applied as follows:
    for (auto j = 0u; j < block_size; j++) {
        // 1. Set T[ i ] equal to Dequant[ i ][ j ] for i = 0..(n0-1).
        for (auto i = 0u; i < block_size; i++)
            column[i] = dequantized[i * block_size + j];

        // 2. If Lossless is equal to 1, invoke the Inverse WHT process as specified in section 8.7.1.10 with shift equal
        //    to 0.
        if (block_context.frame_context.lossless) {
            TRY(inverse_walsh_hadamard_transform(column, log2_of_block_size, 0));
        } else {
            switch (transform_set.first_transform) {
            case TransformType::DCT:
                // Otherwise, if TxType is equal to DCT_DCT or TxType is equal to DCT_ADST, apply an inverse DCT as
                // follows:
                // 1. Invoke the inverse DCT permutation process as specified in section 8.7.1.2 with the input variable n.
                TRY(inverse_discrete_cosine_transform_array_permutation<log2_of_block_size>(column));
                // 2. Invoke the inverse DCT process as specified in section 8.7.1.3 with the input variable n.
                TRY(inverse_discrete_cosine_transform<log2_of_block_size>(column));
                break;
            case TransformType::ADST:
                // 4. Otherwise (TxType is equal to ADST_DCT or TxType is equal to ADST_ADST), invoke the inverse ADST
                //    process as specified in section 8.7.1.9 with input variable n.
                TRY(inverse_asymmetric_discrete_sine_transform<log2_of_block_size>(column));
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        // 5. If Lossless is equal to 1, set Dequant[ i ][ j ] equal to T[ i ] for i = 0..(n0-1).
        for (auto i = 0u; i < block_size; i++)
            dequantized[i * block_size + j] = column[i];

        // 6. Otherwise (Lossless is equal to 0), set Dequant[ i ][ j ] equal to Round2( T[ i ], Min( 6, n + 2 ) )
        //    for i = 0..(n0-1).
        if (!block_context.frame_context.lossless) {
            for (auto i = 0u; i < block_size; i++) {
                auto index = i * block_size + j;
                dequantized[index] = rounded_right_shift(dequantized[index], min(6, log2_of_block_size + 2));
            }
        }
    }

    return {};
}

DecoderErrorOr<void> Decoder::update_reference_frames(FrameContext const& frame_context)
{
    // This process is invoked as the final step in decoding a frame.
    // The inputs to this process are the samples in the current frame CurrFrame[ plane ][ x ][ y ].
    // The output from this process is an updated set of reference frames and previous motion vectors.
    // The following ordered steps apply:

    // 1. For each value of i from 0 to NUM_REF_FRAMES - 1, the following applies if bit i of refresh_frame_flags
    // is equal to 1 (i.e. if (refresh_frame_flags>>i)&1 is equal to 1):
    for (u8 i = 0; i < NUM_REF_FRAMES; i++) {
        if (frame_context.should_update_reference_frame_at_index(i)) {
            auto& reference_frame = m_parser->m_reference_frames[i];

            // − RefFrameWidth[ i ] is set equal to FrameWidth.
            // − RefFrameHeight[ i ] is set equal to FrameHeight.
            reference_frame.size = frame_context.size();
            // − RefSubsamplingX[ i ] is set equal to subsampling_x.
            reference_frame.subsampling_x = frame_context.color_config.subsampling_x;
            // − RefSubsamplingY[ i ] is set equal to subsampling_y.
            reference_frame.subsampling_y = frame_context.color_config.subsampling_y;
            // − RefBitDepth[ i ] is set equal to BitDepth.
            reference_frame.bit_depth = frame_context.color_config.bit_depth;

            // − FrameStore[ i ][ 0 ][ y ][ x ] is set equal to CurrFrame[ 0 ][ y ][ x ] for x = 0..FrameWidth-1, for y =
            // 0..FrameHeight-1.
            // − FrameStore[ i ][ plane ][ y ][ x ] is set equal to CurrFrame[ plane ][ y ][ x ] for plane = 1..2, for x =
            // 0..((FrameWidth+subsampling_x) >> subsampling_x)-1, for y = 0..((FrameHeight+subsampling_y) >>
            // subsampling_y)-1.

            // FIXME: Frame width is not equal to the buffer's stride. If we store the stride of the buffer with the reference
            //        frame, we can just copy the framebuffer data instead. Alternatively, we should crop the output framebuffer.
            for (auto plane = 0u; plane < 3; plane++) {
                auto width = frame_context.size().width();
                auto height = frame_context.size().height();
                auto stride = frame_context.decoded_size(plane > 0).width();
                if (plane > 0) {
                    width = Subsampling::subsampled_size(frame_context.color_config.subsampling_x, width);
                    height = Subsampling::subsampled_size(frame_context.color_config.subsampling_y, height);
                }

                auto const& original_buffer = get_output_buffer(plane);
                auto& frame_store_buffer = reference_frame.frame_planes[plane];
                auto frame_store_width = width + MV_BORDER * 2;
                auto frame_store_height = height + MV_BORDER * 2;
                frame_store_buffer.resize_and_keep_capacity(frame_store_width * frame_store_height);

                VERIFY(original_buffer.size() >= width * height);
                for (auto destination_y = 0u; destination_y < frame_store_height; destination_y++) {
                    // Offset the source row by the motion vector border and then clamp it to the range of 0...height.
                    // This will create an extended border on the top and bottom of the reference frame to avoid having to bounds check
                    // inter-prediction.
                    auto source_y = min(destination_y >= MV_BORDER ? destination_y - MV_BORDER : 0, height - 1);
                    auto const* source = &original_buffer[source_y * stride];
                    auto* destination = &frame_store_buffer[destination_y * frame_store_width + MV_BORDER];
                    AK::TypedTransfer<RemoveReference<decltype(*destination)>>::copy(destination, source, width);
                }

                for (auto destination_y = 0u; destination_y < frame_store_height; destination_y++) {
                    // Stretch the leftmost samples out into the border.
                    auto sample = frame_store_buffer[destination_y * frame_store_width + MV_BORDER];

                    for (auto destination_x = 0u; destination_x < MV_BORDER; destination_x++) {
                        frame_store_buffer[destination_y * frame_store_width + destination_x] = sample;
                    }

                    // Stretch the rightmost samples out into the border.
                    sample = frame_store_buffer[destination_y * frame_store_width + MV_BORDER + width - 1];

                    for (auto destination_x = MV_BORDER + width; destination_x < frame_store_width; destination_x++) {
                        frame_store_buffer[destination_y * frame_store_width + destination_x] = sample;
                    }
                }
            }
        }
    }

    // 2. If show_existing_frame is equal to 0, the following applies:
    if (!frame_context.shows_existing_frame()) {
        DECODER_TRY_ALLOC(m_parser->m_previous_block_contexts.try_resize_to_match_other_vector2d(frame_context.block_contexts()));
        // − PrevRefFrames[ row ][ col ][ list ] is set equal to RefFrames[ row ][ col ][ list ] for row = 0..MiRows-1,
        // for col = 0..MiCols-1, for list = 0..1.
        // − PrevMvs[ row ][ col ][ list ][ comp ] is set equal to Mvs[ row ][ col ][ list ][ comp ] for row = 0..MiRows-1,
        // for col = 0..MiCols-1, for list = 0..1, for comp = 0..1.
        // And from decode_frame():
        // - If all of the following conditions are true, PrevSegmentIds[ row ][ col ] is set equal to
        // SegmentIds[ row ][ col ] for row = 0..MiRows-1, for col = 0..MiCols-1:
        //   − show_existing_frame is equal to 0,
        //   − segmentation_enabled is equal to 1,
        //   − segmentation_update_map is equal to 1.
        bool keep_segment_ids = !frame_context.shows_existing_frame() && frame_context.segmentation_enabled && frame_context.use_full_segment_id_tree;
        frame_context.block_contexts().copy_to(m_parser->m_previous_block_contexts, [keep_segment_ids](FrameBlockContext context) {
            auto persistent_context = PersistentBlockContext(context);
            if (!keep_segment_ids)
                persistent_context.segment_id = 0;
            return persistent_context;
        });
    }

    return {};
}

}
