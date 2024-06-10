/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SyntaxElementCounter.h"
#include <AK/Format.h>

namespace Media::Video::VP9 {

SyntaxElementCounter::SyntaxElementCounter()
{
    __builtin_memset(m_counts_intra_mode, 0, sizeof(m_counts_intra_mode));
    __builtin_memset(m_counts_uv_mode, 0, sizeof(m_counts_uv_mode));
    __builtin_memset(m_counts_partition, 0, sizeof(m_counts_partition));
    __builtin_memset(m_counts_interp_filter, 0, sizeof(m_counts_interp_filter));
    __builtin_memset(m_counts_inter_mode, 0, sizeof(m_counts_inter_mode));
    __builtin_memset(m_counts_tx_size, 0, sizeof(m_counts_tx_size));
    __builtin_memset(m_counts_is_inter, 0, sizeof(m_counts_is_inter));
    __builtin_memset(m_counts_comp_mode, 0, sizeof(m_counts_comp_mode));
    __builtin_memset(m_counts_single_ref, 0, sizeof(m_counts_single_ref));
    __builtin_memset(m_counts_comp_ref, 0, sizeof(m_counts_comp_ref));
    __builtin_memset(m_counts_skip, 0, sizeof(m_counts_skip));
    __builtin_memset(m_counts_mv_joint, 0, sizeof(m_counts_mv_joint));
    __builtin_memset(m_counts_mv_sign, 0, sizeof(m_counts_mv_sign));
    __builtin_memset(m_counts_mv_class, 0, sizeof(m_counts_mv_class));
    __builtin_memset(m_counts_mv_class0_bit, 0, sizeof(m_counts_mv_class0_bit));
    __builtin_memset(m_counts_mv_class0_fr, 0, sizeof(m_counts_mv_class0_fr));
    __builtin_memset(m_counts_mv_class0_hp, 0, sizeof(m_counts_mv_class0_hp));
    __builtin_memset(m_counts_mv_bits, 0, sizeof(m_counts_mv_bits));
    __builtin_memset(m_counts_mv_fr, 0, sizeof(m_counts_mv_fr));
    __builtin_memset(m_counts_mv_hp, 0, sizeof(m_counts_mv_hp));
    __builtin_memset(m_counts_token, 0, sizeof(m_counts_token));
    __builtin_memset(m_counts_more_coefs, 0, sizeof(m_counts_more_coefs));
}

template<typename T, size_t size>
static void sum_arrays(T (&destination)[size], T const (&left)[size], T const (&right)[size])
{
    for (size_t i = 0; i < size; i++) {
        destination[i] = left[i] + right[i];
    }
}

template<typename T, size_t size, size_t size_2>
static void sum_arrays(T (&destination)[size][size_2], T const (&left)[size][size_2], T const (&right)[size][size_2])
{
    for (size_t i = 0; i < size; i++) {
        sum_arrays(destination[i], left[i], right[i]);
    }
}

SyntaxElementCounter SyntaxElementCounter::operator+(SyntaxElementCounter const& other) const
{
    SyntaxElementCounter result;
    sum_arrays(result.m_counts_intra_mode, this->m_counts_intra_mode, other.m_counts_intra_mode);
    sum_arrays(result.m_counts_uv_mode, this->m_counts_uv_mode, other.m_counts_uv_mode);
    sum_arrays(result.m_counts_partition, this->m_counts_partition, other.m_counts_partition);
    sum_arrays(result.m_counts_interp_filter, this->m_counts_interp_filter, other.m_counts_interp_filter);
    sum_arrays(result.m_counts_inter_mode, this->m_counts_inter_mode, other.m_counts_inter_mode);
    sum_arrays(result.m_counts_tx_size, this->m_counts_tx_size, other.m_counts_tx_size);
    sum_arrays(result.m_counts_is_inter, this->m_counts_is_inter, other.m_counts_is_inter);
    sum_arrays(result.m_counts_comp_mode, this->m_counts_comp_mode, other.m_counts_comp_mode);
    sum_arrays(result.m_counts_single_ref, this->m_counts_single_ref, other.m_counts_single_ref);
    sum_arrays(result.m_counts_comp_ref, this->m_counts_comp_ref, other.m_counts_comp_ref);
    sum_arrays(result.m_counts_skip, this->m_counts_skip, other.m_counts_skip);
    sum_arrays(result.m_counts_mv_joint, this->m_counts_mv_joint, other.m_counts_mv_joint);
    sum_arrays(result.m_counts_mv_sign, this->m_counts_mv_sign, other.m_counts_mv_sign);
    sum_arrays(result.m_counts_mv_class, this->m_counts_mv_class, other.m_counts_mv_class);
    sum_arrays(result.m_counts_mv_class0_bit, this->m_counts_mv_class0_bit, other.m_counts_mv_class0_bit);
    sum_arrays(result.m_counts_mv_class0_fr, this->m_counts_mv_class0_fr, other.m_counts_mv_class0_fr);
    sum_arrays(result.m_counts_mv_class0_hp, this->m_counts_mv_class0_hp, other.m_counts_mv_class0_hp);
    sum_arrays(result.m_counts_mv_bits, this->m_counts_mv_bits, other.m_counts_mv_bits);
    sum_arrays(result.m_counts_mv_fr, this->m_counts_mv_fr, other.m_counts_mv_fr);
    sum_arrays(result.m_counts_mv_hp, this->m_counts_mv_hp, other.m_counts_mv_hp);
    sum_arrays(result.m_counts_token, this->m_counts_token, other.m_counts_token);
    sum_arrays(result.m_counts_more_coefs, this->m_counts_more_coefs, other.m_counts_more_coefs);
    return result;
}

SyntaxElementCounter& SyntaxElementCounter::operator+=(SyntaxElementCounter const& other)
{
    *this = *this + other;
    return *this;
}

}
