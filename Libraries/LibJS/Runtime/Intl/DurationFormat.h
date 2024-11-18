/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/String.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>
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
        Digital
    };

    enum class ValueStyle {
        Long,
        Short,
        Narrow,
        Numeric,
        TwoDigit,
        Fractional,
    };
    static_assert(to_underlying(ValueStyle::Long) == to_underlying(Unicode::Style::Long));
    static_assert(to_underlying(ValueStyle::Short) == to_underlying(Unicode::Style::Short));
    static_assert(to_underlying(ValueStyle::Narrow) == to_underlying(Unicode::Style::Narrow));

    enum class Display {
        Auto,
        Always
    };

    static constexpr auto relevant_extension_keys()
    {
        // 1.3.3 Internal slots, https://tc39.es/proposal-intl-duration-format/#sec-Intl.DurationFormat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~DurationFormat() override = default;

    void set_locale(String locale) { m_locale = move(locale); }
    String const& locale() const { return m_locale; }

    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }
    String const& numbering_system() const { return m_numbering_system; }

    void set_hours_minutes_separator(String hours_minutes_separator) { m_hours_minutes_separator = move(hours_minutes_separator); }
    String const& hours_minutes_separator() const { return m_hours_minutes_separator; }

    void set_minutes_seconds_separator(String minutes_seconds_separator) { m_minutes_seconds_separator = move(minutes_seconds_separator); }
    String const& minutes_seconds_separator() const { return m_minutes_seconds_separator; }

    void set_two_digit_hours(bool two_digit_hours) { m_two_digit_hours = two_digit_hours; }
    bool two_digit_hours() const { return m_two_digit_hours; }

    void set_style(StringView style) { m_style = style_from_string(style); }
    Style style() const { return m_style; }
    StringView style_string() const { return style_to_string(m_style); }

    void set_years_style(StringView years_style) { m_years_style = date_style_from_string(years_style); }
    ValueStyle years_style() const { return m_years_style; }
    StringView years_style_string() const { return value_style_to_string(m_years_style); }

    void set_years_display(StringView years_display) { m_years_display = display_from_string(years_display); }
    Display years_display() const { return m_years_display; }
    StringView years_display_string() const { return display_to_string(m_years_display); }

    void set_months_style(StringView months_style) { m_months_style = date_style_from_string(months_style); }
    ValueStyle months_style() const { return m_months_style; }
    StringView months_style_string() const { return value_style_to_string(m_months_style); }

    void set_months_display(StringView months_display) { m_months_display = display_from_string(months_display); }
    Display months_display() const { return m_months_display; }
    StringView months_display_string() const { return display_to_string(m_months_display); }

    void set_weeks_style(StringView weeks_style) { m_weeks_style = date_style_from_string(weeks_style); }
    ValueStyle weeks_style() const { return m_weeks_style; }
    StringView weeks_style_string() const { return value_style_to_string(m_weeks_style); }

    void set_weeks_display(StringView weeks_display) { m_weeks_display = display_from_string(weeks_display); }
    Display weeks_display() const { return m_weeks_display; }
    StringView weeks_display_string() const { return display_to_string(m_weeks_display); }

    void set_days_style(StringView days_style) { m_days_style = date_style_from_string(days_style); }
    ValueStyle days_style() const { return m_days_style; }
    StringView days_style_string() const { return value_style_to_string(m_days_style); }

    void set_days_display(StringView days_display) { m_days_display = display_from_string(days_display); }
    Display days_display() const { return m_days_display; }
    StringView days_display_string() const { return display_to_string(m_days_display); }

    void set_hours_style(StringView hours_style) { m_hours_style = time_style_from_string(hours_style); }
    ValueStyle hours_style() const { return m_hours_style; }
    StringView hours_style_string() const { return value_style_to_string(m_hours_style); }

    void set_hours_display(StringView hours_display) { m_hours_display = display_from_string(hours_display); }
    Display hours_display() const { return m_hours_display; }
    StringView hours_display_string() const { return display_to_string(m_hours_display); }

    void set_minutes_style(StringView minutes_style) { m_minutes_style = time_style_from_string(minutes_style); }
    ValueStyle minutes_style() const { return m_minutes_style; }
    StringView minutes_style_string() const { return value_style_to_string(m_minutes_style); }

    void set_minutes_display(StringView minutes_display) { m_minutes_display = display_from_string(minutes_display); }
    Display minutes_display() const { return m_minutes_display; }
    StringView minutes_display_string() const { return display_to_string(m_minutes_display); }

    void set_seconds_style(StringView seconds_style) { m_seconds_style = time_style_from_string(seconds_style); }
    ValueStyle seconds_style() const { return m_seconds_style; }
    StringView seconds_style_string() const { return value_style_to_string(m_seconds_style); }

    void set_seconds_display(StringView seconds_display) { m_seconds_display = display_from_string(seconds_display); }
    Display seconds_display() const { return m_seconds_display; }
    StringView seconds_display_string() const { return display_to_string(m_seconds_display); }

    void set_milliseconds_style(StringView milliseconds_style) { m_milliseconds_style = sub_second_style_from_string(milliseconds_style); }
    ValueStyle milliseconds_style() const { return m_milliseconds_style; }
    StringView milliseconds_style_string() const { return value_style_to_string(m_milliseconds_style); }

    void set_milliseconds_display(StringView milliseconds_display) { m_milliseconds_display = display_from_string(milliseconds_display); }
    Display milliseconds_display() const { return m_milliseconds_display; }
    StringView milliseconds_display_string() const { return display_to_string(m_milliseconds_display); }

    void set_microseconds_style(StringView microseconds_style) { m_microseconds_style = sub_second_style_from_string(microseconds_style); }
    ValueStyle microseconds_style() const { return m_microseconds_style; }
    StringView microseconds_style_string() const { return value_style_to_string(m_microseconds_style); }

    void set_microseconds_display(StringView microseconds_display) { m_microseconds_display = display_from_string(microseconds_display); }
    Display microseconds_display() const { return m_microseconds_display; }
    StringView microseconds_display_string() const { return display_to_string(m_microseconds_display); }

    void set_nanoseconds_style(StringView nanoseconds_style) { m_nanoseconds_style = sub_second_style_from_string(nanoseconds_style); }
    ValueStyle nanoseconds_style() const { return m_nanoseconds_style; }
    StringView nanoseconds_style_string() const { return value_style_to_string(m_nanoseconds_style); }

    void set_nanoseconds_display(StringView nanoseconds_display) { m_nanoseconds_display = display_from_string(nanoseconds_display); }
    Display nanoseconds_display() const { return m_nanoseconds_display; }
    StringView nanoseconds_display_string() const { return display_to_string(m_nanoseconds_display); }

    void set_fractional_digits(Optional<u8> fractional_digits) { m_fractional_digits = move(fractional_digits); }
    bool has_fractional_digits() const { return m_fractional_digits.has_value(); }
    u8 fractional_digits() const { return m_fractional_digits.value(); }

private:
    explicit DurationFormat(Object& prototype);

    static Style style_from_string(StringView style);
    static StringView style_to_string(Style);
    static ValueStyle date_style_from_string(StringView date_style);
    static ValueStyle time_style_from_string(StringView time_style);
    static ValueStyle sub_second_style_from_string(StringView sub_second_style);
    static StringView value_style_to_string(ValueStyle);
    static Display display_from_string(StringView display);
    static StringView display_to_string(Display);

    String m_locale;                    // [[Locale]]
    String m_numbering_system;          // [[NumberingSystem]]
    String m_hours_minutes_separator;   // [[HourMinutesSeparator]]
    String m_minutes_seconds_separator; // [[MinutesSecondsSeparator]]
    bool m_two_digit_hours { false };   // [[TwoDigitHours]]

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

// 1.1.1 Duration Records, https://tc39.es/proposal-intl-duration-format/#sec-duration-records
struct DurationRecord {
    double years { 0 };
    double months { 0 };
    double weeks { 0 };
    double days { 0 };
    double hours { 0 };
    double minutes { 0 };
    double seconds { 0 };
    double milliseconds { 0 };
    double microseconds { 0 };
    double nanoseconds { 0 };
};

struct DurationInstanceComponent {
    double DurationRecord::*value_slot;
    DurationFormat::ValueStyle (DurationFormat::*get_style_slot)() const;
    void (DurationFormat::*set_style_slot)(StringView);
    DurationFormat::Display (DurationFormat::*get_display_slot)() const;
    void (DurationFormat::*set_display_slot)(StringView);
    StringView unit;
    StringView number_format_unit;
    ReadonlySpan<StringView> values;
    StringView digital_default;
};

// Table 1: Components of Duration Instances, https://tc39.es/proposal-intl-duration-format/#table-duration-component
static constexpr AK::Array<StringView, 3> date_values = { "long"sv, "short"sv, "narrow"sv };
static constexpr AK::Array<StringView, 5> time_values = { "long"sv, "short"sv, "narrow"sv, "numeric"sv, "2-digit"sv };
static constexpr AK::Array<StringView, 4> sub_second_values = { "long"sv, "short"sv, "narrow"sv, "numeric"sv };
static constexpr AK::Array<DurationInstanceComponent, 10> duration_instances_components {
    DurationInstanceComponent { &DurationRecord::years, &DurationFormat::years_style, &DurationFormat::set_years_style, &DurationFormat::years_display, &DurationFormat::set_years_display, "years"sv, "year"sv, date_values, "short"sv },
    DurationInstanceComponent { &DurationRecord::months, &DurationFormat::months_style, &DurationFormat::set_months_style, &DurationFormat::months_display, &DurationFormat::set_months_display, "months"sv, "month"sv, date_values, "short"sv },
    DurationInstanceComponent { &DurationRecord::weeks, &DurationFormat::weeks_style, &DurationFormat::set_weeks_style, &DurationFormat::weeks_display, &DurationFormat::set_weeks_display, "weeks"sv, "week"sv, date_values, "short"sv },
    DurationInstanceComponent { &DurationRecord::days, &DurationFormat::days_style, &DurationFormat::set_days_style, &DurationFormat::days_display, &DurationFormat::set_days_display, "days"sv, "day"sv, date_values, "short"sv },
    DurationInstanceComponent { &DurationRecord::hours, &DurationFormat::hours_style, &DurationFormat::set_hours_style, &DurationFormat::hours_display, &DurationFormat::set_hours_display, "hours"sv, "hour"sv, time_values, "numeric"sv },
    DurationInstanceComponent { &DurationRecord::minutes, &DurationFormat::minutes_style, &DurationFormat::set_minutes_style, &DurationFormat::minutes_display, &DurationFormat::set_minutes_display, "minutes"sv, "minute"sv, time_values, "numeric"sv },
    DurationInstanceComponent { &DurationRecord::seconds, &DurationFormat::seconds_style, &DurationFormat::set_seconds_style, &DurationFormat::seconds_display, &DurationFormat::set_seconds_display, "seconds"sv, "second"sv, time_values, "numeric"sv },
    DurationInstanceComponent { &DurationRecord::milliseconds, &DurationFormat::milliseconds_style, &DurationFormat::set_milliseconds_style, &DurationFormat::milliseconds_display, &DurationFormat::set_milliseconds_display, "milliseconds"sv, "millisecond"sv, sub_second_values, "numeric"sv },
    DurationInstanceComponent { &DurationRecord::microseconds, &DurationFormat::microseconds_style, &DurationFormat::set_microseconds_style, &DurationFormat::microseconds_display, &DurationFormat::set_microseconds_display, "microseconds"sv, "microsecond"sv, sub_second_values, "numeric"sv },
    DurationInstanceComponent { &DurationRecord::nanoseconds, &DurationFormat::nanoseconds_style, &DurationFormat::set_nanoseconds_style, &DurationFormat::nanoseconds_display, &DurationFormat::set_nanoseconds_display, "nanoseconds"sv, "nanosecond"sv, sub_second_values, "numeric"sv },
};

struct DurationUnitOptions {
    String style;
    String display;
};

struct DurationFormatPart {
    StringView type;
    String value;
    StringView unit;
};

ThrowCompletionOr<DurationRecord> to_duration_record(VM&, Value input);
i8 duration_sign(DurationRecord const&);
ThrowCompletionOr<DurationUnitOptions> get_duration_unit_options(VM&, String const& unit, Object const& options, StringView base_style, ReadonlySpan<StringView> styles_list, StringView digital_base, StringView previous_style, bool two_digit_hours);
double add_fractional_digits(DurationFormat const&, DurationRecord const&);
bool next_unit_fractional(DurationFormat const&, StringView unit);
Vector<DurationFormatPart> format_numeric_hours(VM&, DurationFormat const&, double hours_value, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_minutes(VM&, DurationFormat const&, double minutes_value, bool hours_displayed, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_seconds(VM&, DurationFormat const&, double seconds_value, bool minutes_displayed, bool sign_displayed);
Vector<DurationFormatPart> format_numeric_units(VM&, DurationFormat const&, DurationRecord const&, StringView first_numeric_unit, bool sign_displayed);
Vector<DurationFormatPart> list_format_parts(VM&, DurationFormat const&, Vector<Vector<DurationFormatPart>>& partitioned_parts_list);
Vector<DurationFormatPart> partition_duration_format_pattern(VM&, DurationFormat const&, DurationRecord const&);

}
