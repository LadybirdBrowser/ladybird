/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Web::CSS {

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, StyleNodeID, CastToBool, CastToUnderlying, Comparison);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, StyleAtomID, CastToBool, CastToUnderlying, Comparison);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, StyleEngineRuleID, CastToBool, CastToUnderlying, Comparison);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, SheetID, CastToBool, CastToUnderlying, Comparison);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, TreeScopeID, CastToBool, CastToUnderlying, Comparison, Increment);

static_assert(sizeof(StyleNodeID) == sizeof(u32));
static_assert(alignof(StyleNodeID) == alignof(u32));
static_assert(sizeof(StyleAtomID) == sizeof(u32));
static_assert(alignof(StyleAtomID) == alignof(u32));
static_assert(sizeof(StyleEngineRuleID) == sizeof(u32));
static_assert(alignof(StyleEngineRuleID) == alignof(u32));
static_assert(sizeof(SheetID) == sizeof(u32));
static_assert(alignof(SheetID) == alignof(u32));
static_assert(sizeof(TreeScopeID) == sizeof(u32));
static_assert(alignof(TreeScopeID) == alignof(u32));

}
