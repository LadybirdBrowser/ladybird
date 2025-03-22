/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/String.h>
#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

class DurationFormat final : public Object {
    JS_OBJECT(DurationFormat, Object);
    GC_DECLARE_ALLOCATOR(DurationFormat);

public:
    enum class Style {
        Long,
        Short,
        Narrow,
        Digital,
    };
    static Style style_from_string(StringView style);
    static StringView style_to_string(Style);

    enum class ValueStyle {
        Long,
        Short,
        Narrow,
        Numeric,
        TwoDigit,
        Fractional,
    };
    static ValueStyle value_style_from_string(StringView);
    static StringView value_style_to_string(ValueStyle);

    static_assert(to_underlying(ValueStyle::Long) == to_underlying(Unicode::Style::Long));
    static_assert(to_underlying(ValueStyle::Short) == to_underlying(Unicode::Style::Short));
    static_assert(to_underlying(ValueStyle::Narrow) == to_underlying(Unicode::Style::Narrow));

    enum class Display {
        Auto,
        Always,
    };
    static Display display_from_string(StringView display);
    static StringView display_to_string(Display);

    enum class Unit {
        Years,
        Months,
        Weeks,
        Days,
        Hours,
        Minutes,
        Seconds,
        Milliseconds,
        Microseconds,
        Nanoseconds,
    };

    // 13.5.6.1 Duration Unit Options Records, https://tc39.es/ecma402/#sec-durationformat-unit-options-record
    struct DurationUnitOptions {
        ValueStyle style { ValueStyle::Long };
        Display display { Display::Auto };
    };

    static constexpr auto relevant_extension_keys()
    {
        // 13.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.DurationFormat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~DurationFormat() override = default;

    void set_locale(String locale) { m_locale = move(locale); }
    String const& locale() const { return m_locale; }

    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }
    String const& numbering_system() const { return m_numbering_system; }

    void set_hour_minute_separator(String hour_minute_separator) { m_hour_minute_separator = move(hour_minute_separator); }
    String const& hour_minute_separator() const { return m_hour_minute_separator; }

    void set_minute_second_separator(String minute_second_separator) { m_minute_second_separator = move(minute_second_separator); }
    String const& minute_second_separator() const { return m_minute_second_separator; }

    void set_style(StringView style) { m_style = style_from_string(style); }
    Style style() const { return m_style; }
    StringView style_string() const { return style_to_string(m_style); }

    void set_years_options(DurationUnitOptions years_options) { m_years_options = years_options; }
    DurationUnitOptions years_options() const { return m_years_options; }

    void set_months_options(DurationUnitOptions months_options) { m_months_options = months_options; }
    DurationUnitOptions months_options() const { return m_months_options; }

    void set_weeks_options(DurationUnitOptions weeks_options) { m_weeks_options = weeks_options; }
    DurationUnitOptions weeks_options() const { return m_weeks_options; }

    void set_days_options(DurationUnitOptions days_options) { m_days_options = days_options; }
    DurationUnitOptions days_options() const { return m_days_options; }

    void set_hours_options(DurationUnitOptions hours_options) { m_hours_options = hours_options; }
    DurationUnitOptions hours_options() const { return m_hours_options; }

    void set_minutes_options(DurationUnitOptions minutes_options) { m_minutes_options = minutes_options; }
    DurationUnitOptions minutes_options() const { return m_minutes_options; }

    void set_seconds_options(DurationUnitOptions seconds_options) { m_seconds_options = seconds_options; }
    DurationUnitOptions seconds_options() const { return m_seconds_options; }

    void set_milliseconds_options(DurationUnitOptions milliseconds_options) { m_milliseconds_options = milliseconds_options; }
    DurationUnitOptions milliseconds_options() const { return m_milliseconds_options; }

    void set_microseconds_options(DurationUnitOptions microseconds_options) { m_microseconds_options = microseconds_options; }
    DurationUnitOptions microseconds_options() const { return m_microseconds_options; }

    void set_nanoseconds_options(DurationUnitOptions nanoseconds_options) { m_nanoseconds_options = nanoseconds_options; }
    DurationUnitOptions nanoseconds_options() const { return m_nanoseconds_options; }

    void set_fractional_digits(Optional<u8> fractional_digits) { m_fractional_digits = move(fractional_digits); }
    bool has_fractional_digits() const { return m_fractional_digits.has_value(); }
    u8 fractional_digits() const { return m_fractional_digits.value(); }

private:
    explicit DurationFormat(Object& prototype);

    String m_locale;                  // [[Locale]]
    String m_numbering_system;        // [[NumberingSystem]]
    String m_hour_minute_separator;   // [[HourMinutesSeparator]]
    String m_minute_second_separator; // [[MinutesSecondsSeparator]]

    Style m_style { Style::Long };              // [[Style]]
    DurationUnitOptions m_years_options;        // [[YearsOptions]]
    DurationUnitOptions m_months_options;       // [[MonthsOptions]]
    DurationUnitOptions m_weeks_options;        // [[WeeksOptions]]
    DurationUnitOptions m_days_options;         // [[DaysOptions]]
    DurationUnitOptions m_hours_options;        // [[HoursOptions]]
    DurationUnitOptions m_minutes_options;      // [[MinutesOptions]]
    DurationUnitOptions m_seconds_options;      // [[SecondsOptions]]
    DurationUnitOptions m_milliseconds_options; // [[MillisecondsOptions]]
    DurationUnitOptions m_microseconds_options; // [[MicrosecondsOptions]]
    DurationUnitOptions m_nanoseconds_options;  // [[NanosecondsOptions]]
    Optional<u8> m_fractional_digits;           // [[FractionalDigits]]
};

struct DurationInstanceComponent {
    double (Temporal::Duration::*value_slot)() const;
    DurationFormat::DurationUnitOptions (DurationFormat::*get_internal_slot)() const;
    void (DurationFormat::*set_internal_slot)(DurationFormat::DurationUnitOptions);
    DurationFormat::Unit unit;
    ReadonlySpan<StringView> styles;
    DurationFormat::ValueStyle digital_default;
};

// Table 20: Internal slots and property names of DurationFormat instances relevant to Intl.DurationFormat constructor, https://tc39.es/ecma402/#table-durationformat
// Table 24: DurationFormat instance internal slots and properties relevant to PartitionDurationFormatPattern, https://tc39.es/ecma402/#table-partition-duration-format-pattern
static constexpr auto date_styles = AK::Array { "long"sv, "short"sv, "narrow"sv };
static constexpr auto time_styles = AK::Array { "long"sv, "short"sv, "narrow"sv, "numeric"sv, "2-digit"sv };
static constexpr auto sub_second_styles = AK::Array { "long"sv, "short"sv, "narrow"sv, "numeric"sv };

static constexpr auto duration_instances_components = to_array<DurationInstanceComponent>({
    { &Temporal::Duration::years, &DurationFormat::years_options, &DurationFormat::set_years_options, DurationFormat::Unit::Years, date_styles, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::months, &DurationFormat::months_options, &DurationFormat::set_months_options, DurationFormat::Unit::Months, date_styles, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::weeks, &DurationFormat::weeks_options, &DurationFormat::set_weeks_options, DurationFormat::Unit::Weeks, date_styles, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::days, &DurationFormat::days_options, &DurationFormat::set_days_options, DurationFormat::Unit::Days, date_styles, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::hours, &DurationFormat::hours_options, &DurationFormat::set_hours_options, DurationFormat::Unit::Hours, time_styles, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::minutes, &DurationFormat::minutes_options, &DurationFormat::set_minutes_options, DurationFormat::Unit::Minutes, time_styles, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::seconds, &DurationFormat::seconds_options, &DurationFormat::set_seconds_options, DurationFormat::Unit::Seconds, time_styles, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::milliseconds, &DurationFormat::milliseconds_options, &DurationFormat::set_milliseconds_options, DurationFormat::Unit::Milliseconds, sub_second_styles, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::microseconds, &DurationFormat::microseconds_options, &DurationFormat::set_microseconds_options, DurationFormat::Unit::Microseconds, sub_second_styles, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::nanoseconds, &DurationFormat::nanoseconds_options, &DurationFormat::set_nanoseconds_options, DurationFormat::Unit::Nanoseconds, sub_second_styles, DurationFormat::ValueStyle::Numeric },
});

struct DurationFormatPart {
    StringView type;
    String value;
    StringView unit;
};

ThrowCompletionOr<DurationFormat::DurationUnitOptions> get_duration_unit_options(VM&, DurationFormat::Unit unit, Object const& options, DurationFormat::Style base_style, ReadonlySpan<StringView> styles_list, DurationFormat::ValueStyle digital_base, Optional<DurationFormat::ValueStyle> previous_style, bool two_digit_hours);
Crypto::BigFraction compute_fractional_digits(DurationFormat const&, Temporal::Duration const&);
bool next_unit_fractional(DurationFormat const&, DurationFormat::Unit unit);
Vector<DurationFormatPart> format_numeric_hours(VM&, DurationFormat const&, MathematicalValue const& hours_value, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_minutes(VM&, DurationFormat const&, MathematicalValue const& minutes_value, bool hours_displayed, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_seconds(VM&, DurationFormat const&, MathematicalValue const& seconds_value, bool minutes_displayed, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_units(VM&, DurationFormat const&, Temporal::Duration const&, DurationFormat::Unit first_numeric_unit, bool sign_displayed);
bool is_fractional_second_unit_name(DurationFormat::Unit);
Vector<DurationFormatPart> list_format_parts(VM&, DurationFormat const&, Vector<Vector<DurationFormatPart>>& partitioned_parts_list);
Vector<DurationFormatPart> partition_duration_format_pattern(VM&, DurationFormat const&, Temporal::Duration const&);

}
