/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Locale {

enum class CalendarFormatType : u8;
enum class CalendarPatternStyle : u8;
enum class CalendarSymbol : u8;
enum class CompactNumberFormatType : u8;
enum class DayPeriod : u8;
enum class Era : u8;
enum class FirstDayRegion : u8;
enum class HourCycle : u8;
enum class HourCycleRegion : u16;
enum class Key : u8;
enum class KeywordCalendar : u8;
enum class KeywordCollation : u8;
enum class KeywordColCaseFirst : u8;
enum class KeywordColNumeric : u8;
enum class KeywordHours : u8;
enum class KeywordNumbers : u8;
enum class ListPatternType : u8;
enum class Locale : u16;
enum class MinimumDaysRegion : u8;
enum class Month : u8;
enum class NumericSymbol : u8;
enum class PluralCategory : u8;
enum class StandardNumberFormatType : u8;
enum class Style : u8;
enum class Weekday : u8;
enum class WeekendEndRegion : u8;
enum class WeekendStartRegion : u8;

struct CalendarFormat;
struct CalendarPattern;
struct CalendarRangePattern;
struct Keyword;
struct LanguageID;
struct ListPatterns;
struct LocaleExtension;
struct LocaleID;
struct NumberFormat;
struct NumberGroupings;
struct OtherExtension;
struct PluralOperands;
struct TransformedExtension;
struct TransformedField;

}
