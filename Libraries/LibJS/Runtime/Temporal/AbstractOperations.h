/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS::Temporal {

enum class ArithmeticOperation {
    Add,
    Subtract,
};

enum class DateType {
    Date,
    MonthDay,
    YearMonth,
};

enum class Direction {
    Next,
    Previous,
};

enum class Disambiguation {
    Compatible,
    Earlier,
    Later,
    Reject,
};

enum class DurationOperation {
    Since,
    Until,
};

enum class OffsetOption {
    Prefer,
    Use,
    Ignore,
    Reject,
};

enum class Overflow {
    Constrain,
    Reject,
};

enum class ShowCalendar {
    Auto,
    Always,
    Never,
    Critical,
};

enum class ShowOffset {
    Auto,
    Never,
};

enum class ShowTimeZoneName {
    Auto,
    Never,
    Critical,
};

enum class TimeStyle {
    Separated,
    Unseparated,
};

// https://tc39.es/proposal-temporal/#sec-temporal-units
enum class Unit {
    Year,
    Month,
    Week,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
    Microsecond,
    Nanosecond,
};
StringView temporal_unit_to_string(Unit);

// https://tc39.es/proposal-temporal/#sec-temporal-units
enum class UnitCategory {
    Date,
    Time,
};

// https://tc39.es/proposal-temporal/#sec-temporal-units
enum class UnitGroup {
    Date,
    Time,
    DateTime,
};

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
enum class UnsignedRoundingMode {
    HalfEven,
    HalfInfinity,
    HalfZero,
    Infinity,
    Zero,
};

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
enum class Sign {
    Negative,
    Positive,
};

struct Auto { };
struct Unset { };
using Precision = Variant<Auto, u8>;
using RoundingIncrement = Variant<Unset, u64>;
using UnitDefault = Variant<Required, Unset, Auto, Unit>;
using UnitValue = Variant<Unset, Auto, Unit>;

struct SecondsStringPrecision {
    struct Minute { };
    using Precision = Variant<Minute, Auto, u8>;

    Precision precision;
    Unit unit;
    u8 increment { 0 };
};

struct RelativeTo {
    GC::Ptr<PlainDate> plain_relative_to;     // [[PlainRelativeTo]]
    GC::Ptr<ZonedDateTime> zoned_relative_to; // [[ZonedRelativeTo]]
};

struct DifferenceSettings {
    Unit smallest_unit;
    Unit largest_unit;
    RoundingMode rounding_mode;
    u64 rounding_increment { 0 };
};

double iso_date_to_epoch_days(double year, double month, double date);
double epoch_days_to_epoch_ms(double day, double time);
ThrowCompletionOr<void> check_iso_days_range(VM&, ISODate);
ThrowCompletionOr<Overflow> get_temporal_overflow_option(VM&, Object const& options);
ThrowCompletionOr<Disambiguation> get_temporal_disambiguation_option(VM&, Object const& options);
RoundingMode negate_rounding_mode(RoundingMode);
ThrowCompletionOr<OffsetOption> get_temporal_offset_option(VM&, Object const& options, OffsetOption fallback);
ThrowCompletionOr<ShowTimeZoneName> get_temporal_show_time_zone_name_option(VM&, Object const& options);
ThrowCompletionOr<ShowOffset> get_temporal_show_offset_option(VM&, Object const& options);
ThrowCompletionOr<ShowCalendar> get_temporal_show_calendar_name_option(VM&, Object const& options);
ThrowCompletionOr<Direction> get_direction_option(VM&, Object const& options);
ThrowCompletionOr<void> validate_temporal_rounding_increment(VM&, u64 increment, u64 dividend, bool inclusive);
ThrowCompletionOr<Precision> get_temporal_fractional_second_digits_option(VM&, Object const& options);
SecondsStringPrecision to_seconds_string_precision_record(UnitValue, Precision);
ThrowCompletionOr<UnitValue> get_temporal_unit_valued_option(VM&, Object const& options, PropertyKey const&, UnitGroup, UnitDefault const&, ReadonlySpan<UnitValue> extra_values = {});
ThrowCompletionOr<RelativeTo> get_temporal_relative_to_option(VM&, Object const& options);
Unit larger_of_two_temporal_units(Unit, Unit);
bool is_calendar_unit(Unit);
UnitCategory temporal_unit_category(Unit);
RoundingIncrement maximum_temporal_duration_rounding_increment(Unit);
Crypto::UnsignedBigInteger const& temporal_unit_length_in_nanoseconds(Unit);
ThrowCompletionOr<bool> is_partial_temporal_object(VM&, Value);
String format_fractional_seconds(u64, Precision);
String format_time_string(u8 hour, u8 minute, u8 second, u64 sub_second_nanoseconds, SecondsStringPrecision::Precision, Optional<TimeStyle> = {});
UnsignedRoundingMode get_unsigned_rounding_mode(RoundingMode, Sign);
double apply_unsigned_rounding_mode(double, double r1, double r2, UnsignedRoundingMode);
Crypto::SignedBigInteger apply_unsigned_rounding_mode(Crypto::SignedDivisionResult const&, Crypto::SignedBigInteger r1, Crypto::SignedBigInteger r2, UnsignedRoundingMode, Crypto::UnsignedBigInteger const& increment);
double round_number_to_increment(double, u64 increment, RoundingMode);
Crypto::SignedBigInteger round_number_to_increment(Crypto::SignedBigInteger const&, Crypto::UnsignedBigInteger const& increment, RoundingMode);
Crypto::SignedBigInteger round_number_to_increment_as_if_positive(Crypto::SignedBigInteger const&, Crypto::UnsignedBigInteger const& increment, RoundingMode);
ThrowCompletionOr<ParsedISODateTime> parse_iso_date_time(VM&, StringView iso_string, ReadonlySpan<Production> allowed_formats);
ThrowCompletionOr<String> parse_temporal_calendar_string(VM&, String const&);
ThrowCompletionOr<GC::Ref<Duration>> parse_temporal_duration_string(VM&, StringView iso_string);
ThrowCompletionOr<TimeZone> parse_temporal_time_zone_string(VM&, StringView time_zone_string);
ThrowCompletionOr<String> to_month_code(VM&, Value argument);
ThrowCompletionOr<String> to_offset_string(VM&, Value argument);
CalendarFields iso_date_to_fields(StringView calendar, ISODate, DateType);
ThrowCompletionOr<DifferenceSettings> get_difference_settings(VM&, DurationOperation, Object const& options, UnitGroup, ReadonlySpan<Unit> disallowed_units, Unit fallback_smallest_unit, Unit smallest_largest_default_unit);

// 13.38 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
template<typename... Args>
ThrowCompletionOr<double> to_integer_with_truncation(VM& vm, Value argument, ErrorType error_type, Args&&... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is NaN, +∞𝔽 or -∞𝔽, throw a RangeError exception.
    if (number.is_nan() || number.is_infinity())
        return vm.throw_completion<RangeError>(error_type, forward<Args>(args)...);

    // 3. Return truncate(ℝ(number)).
    return trunc(number.as_double());
}

// 13.38 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
// AD-HOC: We often need to use this AO when we have a parsed StringView. This overload allows callers to avoid creating
//         a PrimitiveString for the primary definition.
template<typename... Args>
ThrowCompletionOr<double> to_integer_with_truncation(VM& vm, StringView argument, ErrorType error_type, Args&&... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = string_to_number(argument);

    // 2. If number is NaN, +∞𝔽 or -∞𝔽, throw a RangeError exception.
    if (isnan(number) || isinf(number))
        return vm.throw_completion<RangeError>(error_type, forward<Args>(args)...);

    // 3. Return truncate(ℝ(number)).
    return trunc(number);
}

// 13.37 ToPositiveIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-topositiveintegerwithtruncation
template<typename... Args>
ThrowCompletionOr<double> to_positive_integer_with_truncation(VM& vm, Value argument, ErrorType error_type, Args&&... args)
{
    // 1. Let integer be ? ToIntegerWithTruncation(argument).
    auto integer = TRY(to_integer_with_truncation(vm, argument, error_type, args...));

    // 2. If integer ≤ 0, throw a RangeError exception.
    if (integer <= 0)
        return vm.throw_completion<RangeError>(error_type, args...);

    // 3. Return integer.
    return integer;
}

}
