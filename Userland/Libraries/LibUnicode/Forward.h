/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Unicode {

enum class BidirectionalClass : u8;
enum class EmojiGroup : u8;

struct CurrencyCode;
struct Emoji;

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, GeneralCategory, CastToUnderlying, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, Property, CastToUnderlying, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, Script, CastToUnderlying, Comparison, Increment);

}
