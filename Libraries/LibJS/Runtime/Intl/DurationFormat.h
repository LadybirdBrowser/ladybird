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

    static constexpr auto relevant_extension_keys()
    {
        // 13.3.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.DurationFormat-internal-slots
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

    void set_years_style(ValueStyle years_style) { m_years_style = years_style; }
    ValueStyle years_style() const { return m_years_style; }
    StringView years_style_string() const { return value_style_to_string(m_years_style); }

    void set_years_display(Display years_display) { m_years_display = years_display; }
    Display years_display() const { return m_years_display; }
    StringView years_display_string() const { return display_to_string(m_years_display); }

    void set_months_style(ValueStyle months_style) { m_months_style = months_style; }
    ValueStyle months_style() const { return m_months_style; }
    StringView months_style_string() const { return value_style_to_string(m_months_style); }

    void set_months_display(Display months_display) { m_months_display = months_display; }
    Display months_display() const { return m_months_display; }
    StringView months_display_string() const { return display_to_string(m_months_display); }

    void set_weeks_style(ValueStyle weeks_style) { m_weeks_style = weeks_style; }
    ValueStyle weeks_style() const { return m_weeks_style; }
    StringView weeks_style_string() const { return value_style_to_string(m_weeks_style); }

    void set_weeks_display(Display weeks_display) { m_weeks_display = weeks_display; }
    Display weeks_display() const { return m_weeks_display; }
    StringView weeks_display_string() const { return display_to_string(m_weeks_display); }

    void set_days_style(ValueStyle days_style) { m_days_style = days_style; }
    ValueStyle days_style() const { return m_days_style; }
    StringView days_style_string() const { return value_style_to_string(m_days_style); }

    void set_days_display(Display days_display) { m_days_display = days_display; }
    Display days_display() const { return m_days_display; }
    StringView days_display_string() const { return display_to_string(m_days_display); }

    void set_hours_style(ValueStyle hours_style) { m_hours_style = hours_style; }
    ValueStyle hours_style() const { return m_hours_style; }
    StringView hours_style_string() const { return value_style_to_string(m_hours_style); }

    void set_hours_display(Display hours_display) { m_hours_display = hours_display; }
    Display hours_display() const { return m_hours_display; }
    StringView hours_display_string() const { return display_to_string(m_hours_display); }

    void set_minutes_style(ValueStyle minutes_style) { m_minutes_style = minutes_style; }
    ValueStyle minutes_style() const { return m_minutes_style; }
    StringView minutes_style_string() const { return value_style_to_string(m_minutes_style); }

    void set_minutes_display(Display minutes_display) { m_minutes_display = minutes_display; }
    Display minutes_display() const { return m_minutes_display; }
    StringView minutes_display_string() const { return display_to_string(m_minutes_display); }

    void set_seconds_style(ValueStyle seconds_style) { m_seconds_style = seconds_style; }
    ValueStyle seconds_style() const { return m_seconds_style; }
    StringView seconds_style_string() const { return value_style_to_string(m_seconds_style); }

    void set_seconds_display(Display seconds_display) { m_seconds_display = seconds_display; }
    Display seconds_display() const { return m_seconds_display; }
    StringView seconds_display_string() const { return display_to_string(m_seconds_display); }

    void set_milliseconds_style(ValueStyle milliseconds_style) { m_milliseconds_style = milliseconds_style; }
    ValueStyle milliseconds_style() const { return m_milliseconds_style; }
    StringView milliseconds_style_string() const { return value_style_to_string(m_milliseconds_style); }

    void set_milliseconds_display(Display milliseconds_display) { m_milliseconds_display = milliseconds_display; }
    Display milliseconds_display() const { return m_milliseconds_display; }
    StringView milliseconds_display_string() const { return display_to_string(m_milliseconds_display); }

    void set_microseconds_style(ValueStyle microseconds_style) { m_microseconds_style = microseconds_style; }
    ValueStyle microseconds_style() const { return m_microseconds_style; }
    StringView microseconds_style_string() const { return value_style_to_string(m_microseconds_style); }

    void set_microseconds_display(Display microseconds_display) { m_microseconds_display = microseconds_display; }
    Display microseconds_display() const { return m_microseconds_display; }
    StringView microseconds_display_string() const { return display_to_string(m_microseconds_display); }

    void set_nanoseconds_style(ValueStyle nanoseconds_style) { m_nanoseconds_style = nanoseconds_style; }
    ValueStyle nanoseconds_style() const { return m_nanoseconds_style; }
    StringView nanoseconds_style_string() const { return value_style_to_string(m_nanoseconds_style); }

    void set_nanoseconds_display(Display nanoseconds_display) { m_nanoseconds_display = nanoseconds_display; }
    Display nanoseconds_display() const { return m_nanoseconds_display; }
    StringView nanoseconds_display_string() const { return display_to_string(m_nanoseconds_display); }

    void set_fractional_digits(Optional<u8> fractional_digits) { m_fractional_digits = move(fractional_digits); }
    bool has_fractional_digits() const { return m_fractional_digits.has_value(); }
    u8 fractional_digits() const { return m_fractional_digits.value(); }

private:
    explicit DurationFormat(Object& prototype);

    String m_locale;                  // [[Locale]]
    String m_numbering_system;        // [[NumberingSystem]]
    String m_hour_minute_separator;   // [[HourMinutesSeparator]]
    String m_minute_second_separator; // [[MinutesSecondsSeparator]]

    Style m_style { Style::Long };                        // [[Style]]
    ValueStyle m_years_style { ValueStyle::Long };        // [[YearsStyle]]
    Display m_years_display { Display::Auto };            // [[YearsDisplay]]
    ValueStyle m_months_style { ValueStyle::Long };       // [[MonthsStyle]]
    Display m_months_display { Display::Auto };           // [[MonthsDisplay]]
    ValueStyle m_weeks_style { ValueStyle::Long };        // [[WeeksStyle]]
    Display m_weeks_display { Display::Auto };            // [[WeeksDisplay]]
    ValueStyle m_days_style { ValueStyle::Long };         // [[DaysStyle]]
    Display m_days_display { Display::Auto };             // [[DaysDisplay]]
    ValueStyle m_hours_style { ValueStyle::Long };        // [[HoursStyle]]
    Display m_hours_display { Display::Auto };            // [[HoursDisplay]]
    ValueStyle m_minutes_style { ValueStyle::Long };      // [[MinutesStyle]]
    Display m_minutes_display { Display::Auto };          // [[MinutesDisplay]]
    ValueStyle m_seconds_style { ValueStyle::Long };      // [[SecondsStyle]]
    Display m_seconds_display { Display::Auto };          // [[SecondsDisplay]]
    ValueStyle m_milliseconds_style { ValueStyle::Long }; // [[MillisecondsStyle]]
    Display m_milliseconds_display { Display::Auto };     // [[MillisecondsDisplay]]
    ValueStyle m_microseconds_style { ValueStyle::Long }; // [[MicrosecondsStyle]]
    Display m_microseconds_display { Display::Auto };     // [[MicrosecondsDisplay]]
    ValueStyle m_nanoseconds_style { ValueStyle::Long };  // [[NanosecondsStyle]]
    Display m_nanoseconds_display { Display::Auto };      // [[NanosecondsDisplay]]
    Optional<u8> m_fractional_digits;                     // [[FractionalDigits]]
};

struct DurationInstanceComponent {
    double (Temporal::Duration::*value_slot)() const;
    DurationFormat::ValueStyle (DurationFormat::*get_style_slot)() const;
    void (DurationFormat::*set_style_slot)(DurationFormat::ValueStyle);
    DurationFormat::Display (DurationFormat::*get_display_slot)() const;
    void (DurationFormat::*set_display_slot)(DurationFormat::Display);
    DurationFormat::Unit unit;
    ReadonlySpan<StringView> values;
    DurationFormat::ValueStyle digital_default;
};

// Table 21: DurationFormat instance internal slots and properties relevant to PartitionDurationFormatPattern, https://tc39.es/ecma402/#table-partition-duration-format-pattern
// Table 22: Internal slots and property names of DurationFormat instances relevant to Intl.DurationFormat constructor, https://tc39.es/ecma402/#table-durationformat
static constexpr auto date_values = AK::Array { "long"sv, "short"sv, "narrow"sv };
static constexpr auto time_values = AK::Array { "long"sv, "short"sv, "narrow"sv, "numeric"sv, "2-digit"sv };
static constexpr auto sub_second_values = AK::Array { "long"sv, "short"sv, "narrow"sv, "numeric"sv };

static constexpr auto duration_instances_components = to_array<DurationInstanceComponent>({
    { &Temporal::Duration::years, &DurationFormat::years_style, &DurationFormat::set_years_style, &DurationFormat::years_display, &DurationFormat::set_years_display, DurationFormat::Unit::Years, date_values, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::months, &DurationFormat::months_style, &DurationFormat::set_months_style, &DurationFormat::months_display, &DurationFormat::set_months_display, DurationFormat::Unit::Months, date_values, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::weeks, &DurationFormat::weeks_style, &DurationFormat::set_weeks_style, &DurationFormat::weeks_display, &DurationFormat::set_weeks_display, DurationFormat::Unit::Weeks, date_values, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::days, &DurationFormat::days_style, &DurationFormat::set_days_style, &DurationFormat::days_display, &DurationFormat::set_days_display, DurationFormat::Unit::Days, date_values, DurationFormat::ValueStyle::Short },
    { &Temporal::Duration::hours, &DurationFormat::hours_style, &DurationFormat::set_hours_style, &DurationFormat::hours_display, &DurationFormat::set_hours_display, DurationFormat::Unit::Hours, time_values, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::minutes, &DurationFormat::minutes_style, &DurationFormat::set_minutes_style, &DurationFormat::minutes_display, &DurationFormat::set_minutes_display, DurationFormat::Unit::Minutes, time_values, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::seconds, &DurationFormat::seconds_style, &DurationFormat::set_seconds_style, &DurationFormat::seconds_display, &DurationFormat::set_seconds_display, DurationFormat::Unit::Seconds, time_values, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::milliseconds, &DurationFormat::milliseconds_style, &DurationFormat::set_milliseconds_style, &DurationFormat::milliseconds_display, &DurationFormat::set_milliseconds_display, DurationFormat::Unit::Milliseconds, sub_second_values, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::microseconds, &DurationFormat::microseconds_style, &DurationFormat::set_microseconds_style, &DurationFormat::microseconds_display, &DurationFormat::set_microseconds_display, DurationFormat::Unit::Microseconds, sub_second_values, DurationFormat::ValueStyle::Numeric },
    { &Temporal::Duration::nanoseconds, &DurationFormat::nanoseconds_style, &DurationFormat::set_nanoseconds_style, &DurationFormat::nanoseconds_display, &DurationFormat::set_nanoseconds_display, DurationFormat::Unit::Nanoseconds, sub_second_values, DurationFormat::ValueStyle::Numeric },
});

struct DurationUnitOptions {
    DurationFormat::ValueStyle style;
    DurationFormat::Display display;
};

struct DurationFormatPart {
    StringView type;
    String value;
    StringView unit;
};

ThrowCompletionOr<DurationUnitOptions> get_duration_unit_options(VM&, DurationFormat::Unit unit, Object const& options, DurationFormat::Style base_style, ReadonlySpan<StringView> styles_list, DurationFormat::ValueStyle digital_base, Optional<DurationFormat::ValueStyle> previous_style, bool two_digit_hours);
Crypto::BigFraction compute_fractional_digits(DurationFormat const&, Temporal::Duration const&);
bool next_unit_fractional(DurationFormat const&, DurationFormat::Unit unit);
Vector<DurationFormatPart> format_numeric_hours(VM&, DurationFormat const&, MathematicalValue const& hours_value, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_minutes(VM&, DurationFormat const&, MathematicalValue const& minutes_value, bool hours_displayed, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_seconds(VM&, DurationFormat const&, MathematicalValue const& seconds_value, bool minutes_displayed, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_units(VM&, DurationFormat const&, Temporal::Duration const&, DurationFormat::Unit first_numeric_unit, bool sign_displayed);
Vector<DurationFormatPart> list_format_parts(VM&, DurationFormat const&, Vector<Vector<DurationFormatPart>>& partitioned_parts_list);
Vector<DurationFormatPart> partition_duration_format_pattern(VM&, DurationFormat const&, Temporal::Duration const&);

}
