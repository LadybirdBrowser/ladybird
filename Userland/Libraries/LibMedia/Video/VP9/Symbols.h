/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson<zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Media::Video::VP9 {

// FIXME: These should be placed in logical groupings based on the
//        context they are used in, and perhaps split into multiple
//        files. While doing so, as many of these as possible should be
//        renamed to be more human-readable, and most if not all should
//        be constexpr variables rather than preprocessor definitions.

#define REFS_PER_FRAME 3
#define MV_FR_SIZE 4
#define MVREF_NEIGHBORS 8
#define BLOCK_SIZE_GROUPS 4
#define BLOCK_SIZES 13
#define BLOCK_INVALID 14
#define PARTITION_CONTEXTS 16
#define MI_SIZE 8
#define MIN_TILE_WIDTH_B64 4
#define MAX_TILE_WIDTH_B64 64
#define MAX_MV_REF_CANDIDATES 2
#define LOG2_OF_NUM_REF_FRAMES 3
#define NUM_REF_FRAMES 1 << LOG2_OF_NUM_REF_FRAMES
#define MAX_REF_FRAMES 4
#define IS_INTER_CONTEXTS 4
#define COMP_MODE_CONTEXTS 5
#define REF_CONTEXTS 5
#define MAX_SEGMENTS 8
#define BLOCK_TYPES 2
#define REF_TYPES 2
#define COEF_BANDS 6
#define PREV_COEF_CONTEXTS 6
#define UNCONSTRAINED_NODES 3
#define TX_SIZE_CONTEXTS 2
#define SWITCHABLE_FILTERS 3
#define INTERP_FILTER_CONTEXTS 4
#define SKIP_CONTEXTS 3
#define PARTITION_TYPES 4
#define TX_SIZES 4
#define TX_MODES 5
#define MB_MODE_COUNT 14
#define INTRA_MODES 10
#define INTER_MODES 4
#define INTER_MODE_CONTEXTS 7
#define MV_JOINTS 4
#define MV_CLASSES 11
#define CLASS0_SIZE 2
#define MV_OFFSET_BITS 10
#define MAX_PROB 255
#define MAX_MODE_LF_DELTAS 2
#define COMPANDED_MVREF_THRESH 8
#define MAX_LOOP_FILTER 63
#define REF_SCALE_SHIFT 14
// Number of bits of precision when performing inter prediction.
#define SUBPEL_BITS 4
#define SUBPEL_SHIFTS 16
#define SUBPEL_MASK 15
#define MV_BORDER 128
// Value used when clipping motion vectors.
#define INTERP_EXTEND 4
// Value used when clipping motion vectors.
#define BORDERINPIXELS 160
#define MAX_UPDATE_FACTOR 128
#define COUNT_SAT 20
#define BOTH_ZERO 0
#define ZERO_PLUS_PREDICTED 1
#define BOTH_PREDICTED 2
#define NEW_PLUS_NON_INTRA 3
#define BOTH_NEW 4
#define INTRA_PLUS_NON_INTRA 5
#define BOTH_INTRA 6
#define INVALID_CASE 9

}
