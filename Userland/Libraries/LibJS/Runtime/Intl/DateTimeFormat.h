/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibUnicode/DateTimeFormat.h>

namespace JS::Intl {

class DateTimeFormat final : public Object {
    JS_OBJECT(DateTimeFormat, Object);
    GC_DECLARE_ALLOCATOR(DateTimeFormat);

    using Patterns = Unicode::CalendarPattern;

public:
    static constexpr auto relevant_extension_keys()
    {
        // 11.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.datetimeformat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "ca", "hc", "nu" ».
        return AK::Array { "ca"sv, "hc"sv, "nu"sv };
    }

    virtual ~DateTimeFormat() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    String const& calendar() const { return m_calendar; }
    void set_calendar(String calendar) { m_calendar = move(calendar); }

    String const& numbering_system() const { return m_numbering_system; }
    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }

    String const& time_zone() const { return m_time_zone; }
    void set_time_zone(String time_zone) { m_time_zone = move(time_zone); }

    bool has_date_style() const { return m_date_style.has_value(); }
    Optional<Unicode::DateTimeStyle> const& date_style() const { return m_date_style; }
    StringView date_style_string() const { return Unicode::date_time_style_to_string(*m_date_style); }
    void set_date_style(StringView style) { m_date_style = Unicode::date_time_style_from_string(style); }

    bool has_time_style() const { return m_time_style.has_value(); }
    Optional<Unicode::DateTimeStyle> const& time_style() const { return m_time_style; }
    StringView time_style_string() const { return Unicode::date_time_style_to_string(*m_time_style); }
    void set_time_style(StringView style) { m_time_style = Unicode::date_time_style_from_string(style); }

    Unicode::CalendarPattern& date_time_format() { return m_date_time_format; }
    void set_date_time_format(Unicode::CalendarPattern date_time_format) { m_date_time_format = move(date_time_format); }

    NativeFunction* bound_format() const { return m_bound_format; }
    void set_bound_format(NativeFunction* bound_format) { m_bound_format = bound_format; }

    Unicode::DateTimeFormat const& formatter() const { return *m_formatter; }
    void set_formatter(NonnullOwnPtr<Unicode::DateTimeFormat> formatter) { m_formatter = move(formatter); }

private:
    DateTimeFormat(Object& prototype);

    virtual void visit_edges(Visitor&) override;

    String m_locale;                               // [[Locale]]
    String m_calendar;                             // [[Calendar]]
    String m_numbering_system;                     // [[NumberingSystem]]
    String m_time_zone;                            // [[TimeZone]]
    Optional<Unicode::DateTimeStyle> m_date_style; // [[DateStyle]]
    Optional<Unicode::DateTimeStyle> m_time_style; // [[TimeStyle]]
    Unicode::CalendarPattern m_date_time_format;   // [[DateTimeFormat]]
    GC::Ptr<NativeFunction> m_bound_format;        // [[BoundFormat]]

    // Non-standard. Stores the ICU date-time formatter for the Intl object's formatting options.
    OwnPtr<Unicode::DateTimeFormat> m_formatter;
};

ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> format_date_time_pattern(VM&, DateTimeFormat&, double time);
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> partition_date_time_pattern(VM&, DateTimeFormat&, double time);
ThrowCompletionOr<String> format_date_time(VM&, DateTimeFormat&, double time);
ThrowCompletionOr<GC::Ref<Array>> format_date_time_to_parts(VM&, DateTimeFormat&, double time);
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> partition_date_time_range_pattern(VM&, DateTimeFormat&, double start, double end);
ThrowCompletionOr<String> format_date_time_range(VM&, DateTimeFormat&, double start, double end);
ThrowCompletionOr<GC::Ref<Array>> format_date_time_range_to_parts(VM&, DateTimeFormat&, double start, double end);

template<typename Callback>
ThrowCompletionOr<void> for_each_calendar_field(VM& vm, Unicode::CalendarPattern& pattern, Callback&& callback)
{
    constexpr auto narrow_short_long = AK::Array { "narrow"sv, "short"sv, "long"sv };
    constexpr auto two_digit_numeric = AK::Array { "2-digit"sv, "numeric"sv };
    constexpr auto two_digit_numeric_narrow_short_long = AK::Array { "2-digit"sv, "numeric"sv, "narrow"sv, "short"sv, "long"sv };
    constexpr auto time_zone = AK::Array { "short"sv, "long"sv, "shortOffset"sv, "longOffset"sv, "shortGeneric"sv, "longGeneric"sv };

    // Table 6: Components of date and time formats, https://tc39.es/ecma402/#table-datetimeformat-components
    TRY(callback(pattern.weekday, vm.names.weekday, narrow_short_long));
    TRY(callback(pattern.era, vm.names.era, narrow_short_long));
    TRY(callback(pattern.year, vm.names.year, two_digit_numeric));
    TRY(callback(pattern.month, vm.names.month, two_digit_numeric_narrow_short_long));
    TRY(callback(pattern.day, vm.names.day, two_digit_numeric));
    TRY(callback(pattern.day_period, vm.names.dayPeriod, narrow_short_long));
    TRY(callback(pattern.hour, vm.names.hour, two_digit_numeric));
    TRY(callback(pattern.minute, vm.names.minute, two_digit_numeric));
    TRY(callback(pattern.second, vm.names.second, two_digit_numeric));
    TRY(callback(pattern.fractional_second_digits, vm.names.fractionalSecondDigits, Empty {}));
    TRY(callback(pattern.time_zone_name, vm.names.timeZoneName, time_zone));

    return {};
}

}
