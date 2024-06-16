/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Locale {

enum class CalendarPatternStyle;
enum class HourCycle;
enum class PluralCategory;
enum class Style;
enum class Weekday;

class NumberFormat;

struct CalendarPattern;
struct Keyword;
struct LanguageID;
struct ListFormatPart;
struct LocaleExtension;
struct LocaleID;
struct OtherExtension;
struct TransformedExtension;
struct TransformedField;

}
