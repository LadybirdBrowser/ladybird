/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Symbols.h"
#include <AK/Types.h>

namespace Media::Video::VP9 {

class SyntaxElementCounter final {
public:
    SyntaxElementCounter();

    /* (8.3) Clear Counts Process */
    void clear_counts();

    u32 m_counts_intra_mode[BLOCK_SIZE_GROUPS][INTRA_MODES];
    u32 m_counts_uv_mode[INTRA_MODES][INTRA_MODES];
    u32 m_counts_partition[PARTITION_CONTEXTS][PARTITION_TYPES];
    u32 m_counts_interp_filter[INTERP_FILTER_CONTEXTS][SWITCHABLE_FILTERS];
    u32 m_counts_inter_mode[INTER_MODE_CONTEXTS][INTER_MODES];
    u32 m_counts_tx_size[TX_SIZES][TX_SIZE_CONTEXTS][TX_SIZES];
    u32 m_counts_is_inter[IS_INTER_CONTEXTS][2];
    u32 m_counts_comp_mode[COMP_MODE_CONTEXTS][2];
    u32 m_counts_single_ref[REF_CONTEXTS][2][2];
    u32 m_counts_comp_ref[REF_CONTEXTS][2];
    u32 m_counts_skip[SKIP_CONTEXTS][2];
    u32 m_counts_mv_joint[MV_JOINTS];
    u32 m_counts_mv_sign[2][2];
    u32 m_counts_mv_class[2][MV_CLASSES];
    u32 m_counts_mv_class0_bit[2][CLASS0_SIZE];
    u32 m_counts_mv_class0_fr[2][CLASS0_SIZE][MV_FR_SIZE];
    u32 m_counts_mv_class0_hp[2][2];
    u32 m_counts_mv_bits[2][MV_OFFSET_BITS][2];
    u32 m_counts_mv_fr[2][MV_FR_SIZE];
    u32 m_counts_mv_hp[2][2];
    u32 m_counts_token[TX_SIZES][BLOCK_TYPES][REF_TYPES][COEF_BANDS][PREV_COEF_CONTEXTS][UNCONSTRAINED_NODES];
    u32 m_counts_more_coefs[TX_SIZES][BLOCK_TYPES][REF_TYPES][COEF_BANDS][PREV_COEF_CONTEXTS][2];

    SyntaxElementCounter operator+(SyntaxElementCounter const&) const;
    SyntaxElementCounter& operator+=(SyntaxElementCounter const&);
};

}
