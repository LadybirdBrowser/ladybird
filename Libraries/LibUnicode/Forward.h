/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Unicode {

class NumberFormat;
class Segmenter;

struct CalendarDate;
struct CalendarPattern;
struct CurrencyCode;
struct Emoji;
struct ISODate;
struct Keyword;
struct LanguageID;
struct ListFormatPart;
struct LocaleExtension;
struct LocaleID;
struct OtherExtension;
struct TransformedExtension;
struct TransformedField;

enum class BidiClass;
enum class CalendarPatternStyle;
enum class HourCycle;
enum class PluralCategory;
enum class Style;
enum class Weekday;

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, GeneralCategory, CastToUnderlying, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, Property, CastToUnderlying, Comparison, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, Script, CastToUnderlying, Comparison, Increment);

}
