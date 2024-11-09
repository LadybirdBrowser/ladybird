/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Variant.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/TimeZoneMethods.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

enum class ArithmeticOperation {
    Add,
    Subtract,
};

enum class DifferenceOperation {
    Since,
    Until,
};

enum class UnsignedRoundingMode {
    HalfEven,
    HalfInfinity,
    HalfZero,
    Infinity,
    Zero,
};

enum class OptionType {
    Boolean,
    String,
    Number
};

enum class UnitGroup {
    Date,
    Time,
    DateTime,
};

struct TemporalInstant {
    i32 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u16 millisecond;
    u16 microsecond;
    u16 nanosecond;
    Optional<String> time_zone_offset;
};

struct TemporalDate {
    i32 year;
    u8 month;
    u8 day;
    Optional<String> calendar;
};

struct TemporalTime {
    u8 hour;
    u8 minute;
    u8 second;
    u16 millisecond;
    u16 microsecond;
    u16 nanosecond;
    Optional<String> calendar = {};
};

struct TemporalTimeZone {
    bool z;
    Optional<String> offset_string;
    Optional<String> name;
};

struct TemporalYearMonth {
    i32 year;
    u8 month;
    u8 day;
    Optional<String> calendar = {};
};

struct TemporalMonthDay {
    Optional<i32> year;
    u8 month;
    u8 day;
    Optional<String> calendar = {};
};

struct ISODateTime {
    i32 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u16 millisecond;
    u16 microsecond;
    u16 nanosecond;
    TemporalTimeZone time_zone { .z = false, .offset_string = {}, .name = {} };
    Optional<String> calendar = {};
};

struct SecondsStringPrecision {
    Variant<StringView, u8> precision;
    StringView unit;
    u32 increment;
};

struct DifferenceSettings {
    String smallest_unit;
    String largest_unit;
    String rounding_mode;
    u64 rounding_increment;
    NonnullGCPtr<Object> options;
};

struct TemporalUnitRequired { };
struct PrepareTemporalFieldsPartial { };
struct GetOptionRequired { };

using OptionDefault = Variant<GetOptionRequired, Empty, bool, StringView, double>;
using TemporalUnitDefault = Variant<TemporalUnitRequired, Optional<StringView>>;

ThrowCompletionOr<MarkedVector<Value>> iterable_to_list_of_type(VM&, Value items, Vector<OptionType> const& element_types);
ThrowCompletionOr<Object*> get_options_object(VM&, Value options);
ThrowCompletionOr<Value> get_option(VM&, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const&);
ThrowCompletionOr<String> to_temporal_overflow(VM&, Object const* options);
ThrowCompletionOr<String> to_temporal_disambiguation(VM&, Object const* options);
ThrowCompletionOr<String> to_temporal_rounding_mode(VM&, Object const& normalized_options, StringView fallback);
StringView negate_temporal_rounding_mode(StringView rounding_mode);
ThrowCompletionOr<String> to_temporal_offset(VM&, Object const* options, StringView fallback);
ThrowCompletionOr<String> to_calendar_name_option(VM&, Object const& normalized_options);
ThrowCompletionOr<String> to_time_zone_name_option(VM&, Object const& normalized_options);
ThrowCompletionOr<String> to_show_offset_option(VM&, Object const& normalized_options);
ThrowCompletionOr<u64> to_temporal_rounding_increment(VM& vm, Object const& normalized_options);
ThrowCompletionOr<void> validate_temporal_rounding_increment(VM& vm, u64 increment, u64 dividend, bool inclusive);
ThrowCompletionOr<SecondsStringPrecision> to_seconds_string_precision_record(VM&, Object const& normalized_options);
ThrowCompletionOr<Optional<String>> get_temporal_unit(VM&, Object const& normalized_options, PropertyKey const&, UnitGroup, TemporalUnitDefault const& default_, Vector<StringView> const& extra_values = {});

struct RelativeTo {
    GCPtr<PlainDate> plain_relative_to;         // [[PlainRelativeTo]]
    GCPtr<ZonedDateTime> zoned_relative_to;     // [[ZonedRelativeTo]]
    Optional<TimeZoneMethods> time_zone_record; // [[TimeZoneRec]]
};
ThrowCompletionOr<RelativeTo> to_relative_temporal_object(VM&, Object const& options);

Value relative_to_converted_to_value(RelativeTo const&);

StringView larger_of_two_temporal_units(StringView, StringView);
ThrowCompletionOr<Object*> merge_largest_unit_option(VM&, Object const& options, String largest_unit);
Optional<u16> maximum_temporal_duration_rounding_increment(StringView unit);
ThrowCompletionOr<void> reject_object_with_calendar_or_time_zone(VM&, Object&);
ThrowCompletionOr<String> format_seconds_string_part(VM&, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Variant<StringView, u8> const& precision);
double sign(double);
double sign(Crypto::SignedBigInteger const&);
UnsignedRoundingMode get_unsigned_rounding_mode(StringView rounding_mode, bool is_negative);
double apply_unsigned_rounding_mode(double x, double r1, double r2, Optional<UnsignedRoundingMode> const&);
Crypto::SignedBigInteger apply_unsigned_rounding_mode(Crypto::SignedDivisionResult const&, Crypto::SignedBigInteger const& r1, Crypto::SignedBigInteger const& r2, Optional<UnsignedRoundingMode> const&, Crypto::UnsignedBigInteger const& increment);
double round_number_to_increment(double, u64 increment, StringView rounding_mode);
Crypto::SignedBigInteger round_number_to_increment(Crypto::SignedBigInteger const&, u64 increment, StringView rounding_mode);
Crypto::SignedBigInteger round_number_to_increment_as_if_positive(Crypto::SignedBigInteger const&, u64 increment, StringView rounding_mode);
ThrowCompletionOr<ISODateTime> parse_iso_date_time(VM&, StringView iso_string);
ThrowCompletionOr<ISODateTime> parse_iso_date_time(VM&, ParseResult const& parse_result);
ThrowCompletionOr<TemporalInstant> parse_temporal_instant_string(VM&, StringView iso_string);
ThrowCompletionOr<ISODateTime> parse_temporal_zoned_date_time_string(VM&, StringView iso_string);
ThrowCompletionOr<String> parse_temporal_calendar_string(VM&, StringView iso_string);
ThrowCompletionOr<TemporalDate> parse_temporal_date_string(VM&, StringView iso_string);
ThrowCompletionOr<ISODateTime> parse_temporal_date_time_string(VM&, StringView iso_string);
ThrowCompletionOr<DurationRecord> parse_temporal_duration_string(VM&, StringView iso_string);
ThrowCompletionOr<TemporalMonthDay> parse_temporal_month_day_string(VM&, StringView iso_string);
ThrowCompletionOr<ISODateTime> parse_temporal_relative_to_string(VM&, StringView iso_string);
ThrowCompletionOr<TemporalTime> parse_temporal_time_string(VM&, StringView iso_string);
ThrowCompletionOr<TemporalTimeZone> parse_temporal_time_zone_string(VM&, StringView iso_string);
ThrowCompletionOr<TemporalYearMonth> parse_temporal_year_month_string(VM&, StringView iso_string);
ThrowCompletionOr<double> to_positive_integer_with_truncation(VM&, Value argument);
ThrowCompletionOr<Object*> prepare_temporal_fields(VM&, Object const& fields, Vector<String> const& field_names, Variant<PrepareTemporalFieldsPartial, Vector<StringView>> const& required_fields);
ThrowCompletionOr<DifferenceSettings> get_difference_settings(VM&, DifferenceOperation, Value options_value, UnitGroup unit_group, Vector<StringView> const& disallowed_units, TemporalUnitDefault const& fallback_smallest_unit, StringView smallest_largest_default_unit);

template<size_t Size>
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, StringView const (&values)[Size], OptionDefault const& default_)
{
    return get_option(vm, options, property, type, ReadonlySpan<StringView> { values }, default_);
}

// 13.40 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
template<typename... Args>
ThrowCompletionOr<double> to_integer_with_truncation(VM& vm, Value argument, ErrorType error_type, Args... args)
{
    // 1. Let number be ? ToIntegerOrInfinity(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is NaN, return 0.
    if (number.is_nan())
        return 0;

    // 3. If number is +∞𝔽 or -∞𝔽, throw a RangeError exception.
    if (Value(number).is_infinity()) {
        return vm.template throw_completion<RangeError>(error_type, args...);
    }

    // 4. Return truncate(ℝ(number)).
    return trunc(number.as_double());
}

// 13.41 ToIntegerIfIntegral ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerifintegral
template<typename... Args>
ThrowCompletionOr<double> to_integer_if_integral(VM& vm, Value argument, ErrorType error_type, Args... args)
{
    // 1. Let number be ? ToNumber(argument).
    auto number = TRY(argument.to_number(vm));

    // 2. If number is NaN, +0𝔽, or -0𝔽, return 0.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return 0;

    // 3. If IsIntegralNumber(number) is false, throw a RangeError exception.
    if (!number.is_integral_number())
        return vm.template throw_completion<RangeError>(error_type, args...);

    // 4. Return ℝ(number).
    return number.as_double();
}

}
