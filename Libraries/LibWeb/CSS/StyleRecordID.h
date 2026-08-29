/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Web::CSS {

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, StyleRecordID, CastToBool, CastToUnderlying);

inline bool style_record_has_animation_overlay(StyleRecordID style_record)
{
    return (style_record.value() & (1ull << 63)) != 0;
}

}
