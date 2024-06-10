/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BooleanDecoder.h"
#include "ContextStorage.h"
#include "Enums.h"
#include "ProbabilityTables.h"
#include "SyntaxElementCounter.h"

namespace Media::Video::VP9 {

class Parser;

struct BlockContext;
struct FrameBlockContext;

struct TokensContext {
    TransformSize m_tx_size;
    bool m_is_uv_plane;
    bool m_is_inter;
    u8 m_band;
    u8 m_context_index;
};

class TreeParser {
public:
    static Partition parse_partition(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, bool has_rows, bool has_columns, BlockSubsize block_subsize, u8 num_8x8, PartitionContextView above_partition_context, PartitionContextView left_partition_context, u32 row, u32 column, bool frame_is_intra);
    static PredictionMode parse_default_intra_mode(BooleanDecoder&, ProbabilityTables const&, BlockSubsize mi_size, FrameBlockContext above, FrameBlockContext left, Array<PredictionMode, 4> const& block_sub_modes, u8 index_x, u8 index_y);
    static PredictionMode parse_default_uv_mode(BooleanDecoder&, ProbabilityTables const&, PredictionMode y_mode);
    static PredictionMode parse_intra_mode(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, BlockSubsize mi_size);
    static PredictionMode parse_sub_intra_mode(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&);
    static PredictionMode parse_uv_mode(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, PredictionMode y_mode);
    static u8 parse_segment_id(BooleanDecoder&, Array<u8, 7> const& probabilities);
    static bool parse_segment_id_predicted(BooleanDecoder&, Array<u8, 3> const& probabilities, u8 above_seg_pred_context, u8 left_seg_pred_context);
    static PredictionMode parse_inter_mode(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 mode_context_for_ref_frame_0);
    static InterpolationFilter parse_interpolation_filter(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, FrameBlockContext above, FrameBlockContext left);
    static bool parse_skip(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, FrameBlockContext above, FrameBlockContext left);
    static TransformSize parse_tx_size(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, TransformSize max_tx_size, FrameBlockContext above, FrameBlockContext left);
    static bool parse_block_is_inter_predicted(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, FrameBlockContext above, FrameBlockContext left);
    static ReferenceMode parse_comp_mode(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, ReferenceFrameType comp_fixed_ref, FrameBlockContext above, FrameBlockContext left);
    static ReferenceIndex parse_comp_ref(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, ReferenceFrameType comp_fixed_ref, ReferenceFramePair comp_var_ref, ReferenceIndex variable_reference_index, FrameBlockContext above, FrameBlockContext left);
    static bool parse_single_ref_part_1(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, FrameBlockContext above, FrameBlockContext left);
    static bool parse_single_ref_part_2(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, FrameBlockContext above, FrameBlockContext left);

    static MvJoint parse_motion_vector_joint(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&);
    static bool parse_motion_vector_sign(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component);
    static MvClass parse_motion_vector_class(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component);
    static bool parse_motion_vector_class0_bit(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component);
    static u8 parse_motion_vector_class0_fr(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component, bool class_0_bit);
    static bool parse_motion_vector_class0_hp(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component, bool use_hp);
    static bool parse_motion_vector_bit(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component, u8 bit_index);
    static u8 parse_motion_vector_fr(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component);
    static bool parse_motion_vector_hp(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, u8 component, bool use_hp);

    static TokensContext get_context_for_first_token(NonZeroTokensView above_non_zero_tokens, NonZeroTokensView left_non_zero_tokens, TransformSize transform_size, u8 plane, u32 sub_block_column, u32 sub_block_row, bool is_inter, u8 band);
    static TokensContext get_context_for_other_tokens(Array<u8, 1024> token_cache, TransformSize transform_size, TransformSet transform_set, u8 plane, u16 token_position, bool is_inter, u8 band);
    static bool parse_more_coefficients(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, TokensContext const& context);
    static Token parse_token(BooleanDecoder&, ProbabilityTables const&, SyntaxElementCounter&, TokensContext const& context);
};

struct PartitionTreeContext {
    bool has_rows;
    bool has_columns;
    BlockSubsize block_subsize;
    u8 num_8x8;
    Vector<u8> const& above_partition_context;
    Vector<u8> const& left_partition_context;
    u32 row;
    u32 column;
    bool frame_is_intra;
};

}
