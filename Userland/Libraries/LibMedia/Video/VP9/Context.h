/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/BitStream.h>
#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Subsampling.h>

#include "BooleanDecoder.h"
#include "ContextStorage.h"
#include "Enums.h"
#include "LookupTables.h"
#include "MotionVector.h"
#include "SyntaxElementCounter.h"
#include "Utilities.h"

namespace Media::Video::VP9 {

enum class FrameShowMode {
    CreateAndShowNewFrame,
    ShowExistingFrame,
    DoNotShowFrame,
};

struct Quantizers {
    u16 y_ac_quantizer { 0 };
    u16 uv_ac_quantizer { 0 };

    u16 y_dc_quantizer { 0 };
    u16 uv_dc_quantizer { 0 };
};

struct FrameContext {
public:
    static ErrorOr<FrameContext> create(ReadonlyBytes data,
        Vector2D<FrameBlockContext>& contexts)
    {
        return FrameContext(
            data,
            TRY(try_make<FixedMemoryStream>(data)),
            TRY(try_make<SyntaxElementCounter>()),
            contexts);
    }

    FrameContext(FrameContext const&) = delete;
    FrameContext(FrameContext&&) = default;

    ReadonlyBytes stream_data;
    NonnullOwnPtr<FixedMemoryStream> stream;
    BigEndianInputBitStream bit_stream;

    DecoderErrorOr<BooleanDecoder> create_range_decoder(size_t size)
    {
        if (size > stream->remaining())
            return DecoderError::corrupted("Range decoder size invalid"sv);

        auto compressed_header_data = ReadonlyBytes(stream_data.data() + stream->offset(), size);

        // 9.2.1: The Boolean decoding process specified in section 9.2.2 is invoked to read a marker syntax element from the
        //        bitstream. It is a requirement of bitstream conformance that the value read is equal to 0.
        auto decoder = DECODER_TRY(DecoderErrorCategory::Corrupted, BooleanDecoder::initialize(compressed_header_data));
        if (decoder.read_bool(128))
            return DecoderError::corrupted("Range decoder marker was non-zero"sv);

        DECODER_TRY(DecoderErrorCategory::Corrupted, bit_stream.discard(size));
        return decoder;
    }

    NonnullOwnPtr<SyntaxElementCounter> counter;

    u8 profile { 0 };

    FrameType type { FrameType::KeyFrame };
    bool is_inter_predicted() const { return type == FrameType::InterFrame; }

    bool error_resilient_mode { false };
    bool parallel_decoding_mode { false };
    bool should_replace_probability_context { false };

    bool shows_a_frame() const { return m_frame_show_mode != FrameShowMode::DoNotShowFrame; }
    bool shows_a_new_frame() const { return m_frame_show_mode == FrameShowMode::CreateAndShowNewFrame; }
    bool shows_existing_frame() const { return m_frame_show_mode == FrameShowMode::ShowExistingFrame; }
    void set_frame_hidden() { m_frame_show_mode = FrameShowMode::DoNotShowFrame; }
    void set_existing_frame_to_show(u8 index)
    {
        m_frame_show_mode = FrameShowMode::ShowExistingFrame;
        m_existing_frame_index = index;
    }
    u8 existing_frame_index() const { return m_existing_frame_index; }

    bool use_previous_frame_motion_vectors { false };

    ColorConfig color_config {};

    u8 reference_frames_to_update_flags { 0 };
    bool should_update_reference_frame_at_index(u8 index) const { return (reference_frames_to_update_flags & (1 << index)) != 0; }

    u8 probability_context_index { 0 };

    Gfx::Size<u32> size() const { return m_size; }
    ErrorOr<void> set_size(Gfx::Size<u32> size)
    {
        m_size = size;

        // From spec, compute_image_size( )
        m_rows = pixels_to_blocks(size.height() + 7u);
        m_columns = pixels_to_blocks(size.width() + 7u);
        return m_block_contexts.try_resize(m_rows, m_columns);
    }
    u32 rows() const { return m_rows; }
    u32 columns() const { return m_columns; }
    u32 superblock_rows() const { return blocks_ceiled_to_superblocks(rows()); }
    u32 superblock_columns() const { return blocks_ceiled_to_superblocks(columns()); }
    // Calculates the output size for each plane in the frame.
    Gfx::Size<u32> decoded_size(bool uv) const
    {
        if (uv) {
            return {
                Subsampling::subsampled_size(color_config.subsampling_y, blocks_to_pixels(columns())),
                Subsampling::subsampled_size(color_config.subsampling_y, blocks_to_pixels(rows())),
            };
        }
        return {
            blocks_to_pixels(columns()),
            blocks_to_pixels(rows()),
        };
    }

    Vector2D<FrameBlockContext> const& block_contexts() const { return m_block_contexts; }

    Gfx::Size<u32> render_size { 0, 0 };
    Gfx::Size<u16> log2_of_tile_counts { 0, 0 };

    // This group of fields is only needed for inter-predicted frames.
    Array<u8, 3> reference_frame_indices;
    Array<bool, ReferenceFrameType::LastFrame + 3> reference_frame_sign_biases;
    bool high_precision_motion_vectors_allowed { false };
    InterpolationFilter interpolation_filter { InterpolationFilter::Switchable };

    u8 loop_filter_level { 0 };
    u8 loop_filter_sharpness { 0 };
    bool loop_filter_delta_enabled { false };
    Array<i8, MAX_REF_FRAMES> loop_filter_reference_deltas;
    Array<i8, 2> loop_filter_mode_deltas;

    // Set based on quantization_params( ) in the spec.
    bool lossless { false };
    Array<Quantizers, MAX_SEGMENTS> segment_quantizers;

    bool segmentation_enabled { false };
    // Note: We can use Optional<Array<...>> for these tree probabilities, but unfortunately it seems to have measurable performance overhead.
    bool use_full_segment_id_tree { false };
    Array<u8, 7> full_segment_id_tree_probabilities;
    bool use_predicted_segment_id_tree { false };
    Array<u8, 3> predicted_segment_id_tree_probabilities;
    bool should_use_absolute_segment_base_quantizer { false };
    SegmentationFeatures segmentation_features;
    SegmentFeatureStatus get_segment_feature(u8 segment_id, SegmentFeature feature) const
    {
        return segmentation_features[segment_id][to_underlying(feature)];
    }

    u16 header_size_in_bytes { 0 };

    TransformMode transform_mode;

    // This group also is only needed for inter-predicted frames.
    ReferenceMode reference_mode;
    ReferenceFrameType fixed_reference_type;
    ReferenceFramePair variable_reference_types;

private:
    friend struct TileContext;

    FrameContext(ReadonlyBytes data,
        NonnullOwnPtr<FixedMemoryStream> stream,
        NonnullOwnPtr<SyntaxElementCounter> counter,
        Vector2D<FrameBlockContext>& contexts)
        : stream_data(data)
        , stream(move(stream))
        , bit_stream(MaybeOwned<Stream>(*this->stream))
        , counter(move(counter))
        , m_block_contexts(contexts)
    {
    }

    FrameShowMode m_frame_show_mode { FrameShowMode::CreateAndShowNewFrame };
    u8 m_existing_frame_index { 0 };

    Gfx::Size<u32> m_size { 0, 0 };
    u32 m_rows { 0 };
    u32 m_columns { 0 };
    // FIXME: From spec: NOTE – We are using a 2D array to store the SubModes for clarity. It is possible to reduce memory
    //        consumption by only storing one intra mode for each 8x8 horizontal and vertical position, i.e. to use two 1D
    //        arrays instead.
    //        I think should also apply to other fields that are only accessed relative to the current block. Worth looking
    //        into how much of this context needs to be stored for the whole frame vs a row or column from the current tile.
    Vector2D<FrameBlockContext>& m_block_contexts;
};

static ErrorOr<NonZeroTokens> create_non_zero_tokens(u32 size_in_sub_blocks, bool subsampling)
{
    return NonZeroTokens {
        TRY(FixedArray<bool>::create(size_in_sub_blocks)),
        TRY(FixedArray<bool>::create(size_in_sub_blocks >>= subsampling)),
        TRY(FixedArray<bool>::create(size_in_sub_blocks)),
    };
}

template<typename T>
static Span<T> safe_slice(Span<T> span, u32 start, u32 size)
{
    return span.slice(start, min(size, span.size() - start));
}

static NonZeroTokensView create_non_zero_tokens_view(NonZeroTokensView non_zero_tokens, u32 start_in_sub_blocks, u32 size_in_sub_blocks, bool subsampling)
{
    NonZeroTokensView result;
    // Y plane
    result[0] = safe_slice(non_zero_tokens[0], start_in_sub_blocks, size_in_sub_blocks);
    // UV planes
    start_in_sub_blocks >>= subsampling;
    size_in_sub_blocks >>= subsampling;
    result[1] = safe_slice(non_zero_tokens[1], start_in_sub_blocks, size_in_sub_blocks);
    result[2] = safe_slice(non_zero_tokens[2], start_in_sub_blocks, size_in_sub_blocks);
    return result;
}

static NonZeroTokensView create_non_zero_tokens_view(NonZeroTokens& non_zero_tokens, u32 start_in_sub_blocks, u32 size_in_sub_blocks, bool subsampling)
{
    return create_non_zero_tokens_view({ non_zero_tokens[0].span(), non_zero_tokens[1].span(), non_zero_tokens[2].span() }, start_in_sub_blocks, size_in_sub_blocks, subsampling);
}

struct TileContext {
public:
    static DecoderErrorOr<TileContext> try_create(FrameContext& frame_context, u32 tile_size, u32 rows_start, u32 rows_end, u32 columns_start, u32 columns_end, PartitionContextView above_partition_context, NonZeroTokensView above_non_zero_tokens, SegmentationPredictionContextView above_segmentation_ids)
    {
        auto width = columns_end - columns_start;
        auto height = rows_end - rows_start;
        auto context_view = frame_context.m_block_contexts.view(rows_start, columns_start, height, width);

        return TileContext {
            frame_context,
            TRY(frame_context.create_range_decoder(tile_size)),
            DECODER_TRY_ALLOC(try_make<SyntaxElementCounter>()),
            rows_start,
            rows_end,
            columns_start,
            columns_end,
            context_view,
            above_partition_context,
            above_non_zero_tokens,
            above_segmentation_ids,
            DECODER_TRY_ALLOC(PartitionContext::create(superblocks_to_blocks(blocks_ceiled_to_superblocks(height)))),
            DECODER_TRY_ALLOC(create_non_zero_tokens(blocks_to_sub_blocks(height), frame_context.color_config.subsampling_y)),
            DECODER_TRY_ALLOC(SegmentationPredictionContext::create(height)),
        };
    }

    Vector2D<FrameBlockContext> const& frame_block_contexts() const { return frame_context.block_contexts(); }

    FrameContext const& frame_context;
    BooleanDecoder decoder;
    NonnullOwnPtr<SyntaxElementCounter> counter;
    u32 rows_start { 0 };
    u32 rows_end { 0 };
    u32 columns_start { 0 };
    u32 columns_end { 0 };
    u32 rows() const { return rows_end - rows_start; }
    u32 columns() const { return columns_end - columns_start; }
    Vector2DView<FrameBlockContext> block_contexts_view;

    PartitionContextView above_partition_context;
    NonZeroTokensView above_non_zero_tokens;
    SegmentationPredictionContextView above_segmentation_ids;
    PartitionContext left_partition_context;
    NonZeroTokens left_non_zero_tokens;
    SegmentationPredictionContext left_segmentation_ids;
};

struct BlockContext {
    static BlockContext create(TileContext& tile_context, u32 row, u32 column, BlockSubsize size)
    {
        auto contexts_view = tile_context.block_contexts_view.view(
            row - tile_context.rows_start,
            column - tile_context.columns_start,
            min<u32>(num_8x8_blocks_high_lookup[size], tile_context.frame_context.rows() - row),
            min<u32>(num_8x8_blocks_wide_lookup[size], tile_context.frame_context.columns() - column));

        auto size_in_blocks = block_size_to_blocks(size);
        auto size_in_sub_blocks = block_size_to_sub_blocks(get_subsampled_block_size(size, false, false));

        return BlockContext {
            .frame_context = tile_context.frame_context,
            .tile_context = tile_context,
            .decoder = tile_context.decoder,
            .counter = *tile_context.counter,
            .row = row,
            .column = column,
            .size = size,
            .contexts_view = contexts_view,
            .above_non_zero_tokens = create_non_zero_tokens_view(tile_context.above_non_zero_tokens, blocks_to_sub_blocks(column - tile_context.columns_start), size_in_sub_blocks.width(), tile_context.frame_context.color_config.subsampling_x),
            .above_segmentation_ids = safe_slice(tile_context.above_segmentation_ids, column - tile_context.columns_start, size_in_blocks.width()),
            .left_non_zero_tokens = create_non_zero_tokens_view(tile_context.left_non_zero_tokens, blocks_to_sub_blocks(row - tile_context.rows_start), size_in_sub_blocks.height(), tile_context.frame_context.color_config.subsampling_y),
            .left_segmentation_ids = safe_slice(tile_context.left_segmentation_ids.span(), row - tile_context.rows_start, size_in_blocks.height()),
        };
    }

    Vector2D<FrameBlockContext> const& frame_block_contexts() const { return frame_context.block_contexts(); }

    FrameContext const& frame_context;
    TileContext const& tile_context;
    BooleanDecoder& decoder;
    SyntaxElementCounter& counter;
    u32 row { 0 };
    u32 column { 0 };
    BlockSubsize size;
    Gfx::Size<u8> get_size_in_sub_blocks() const
    {
        return block_size_to_sub_blocks(size);
    }

    Vector2DView<FrameBlockContext> contexts_view;

    u8 segment_id { 0 };
    bool should_skip_residuals { false };

    TransformSize transform_size { Transform_4x4 };

    ReferenceFramePair reference_frame_types {};
    bool is_inter_predicted() const { return reference_frame_types.primary != ReferenceFrameType::None; }
    bool is_compound() const { return reference_frame_types.secondary != ReferenceFrameType::None; }

    Array<PredictionMode, 4> sub_block_prediction_modes {};
    PredictionMode y_prediction_mode() const { return sub_block_prediction_modes.last(); }
    PredictionMode& y_prediction_mode() { return sub_block_prediction_modes.last(); }
    PredictionMode uv_prediction_mode { 0 };

    InterpolationFilter interpolation_filter { EightTap };
    Array<MotionVectorPair, 4> sub_block_motion_vectors {};

    Array<i32, 1024> residual_tokens {};

    // Indexed by ReferenceFrame enum.
    Array<u8, 4> mode_context {};

    NonZeroTokensView above_non_zero_tokens;
    SegmentationPredictionContextView above_segmentation_ids;
    NonZeroTokensView left_non_zero_tokens;
    SegmentationPredictionContextView left_segmentation_ids;

    SegmentFeatureStatus get_segment_feature(SegmentFeature feature) const
    {
        return frame_context.get_segment_feature(segment_id, feature);
    }
};

struct BlockMotionVectorCandidateSet {
    MotionVector near_vector;
    MotionVector nearest_vector;
    MotionVector best_vector;
};

struct MotionVectorCandidate {
    ReferenceFrameType type;
    MotionVector vector;
};

}
