/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <LibUnicode/DurationFormat.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/NumberFormat.h>

#include <unicode/measfmt.h>
#include <unicode/measunit.h>
#include <unicode/measure.h>

namespace Unicode {

static constexpr bool is_not_ascii_digit(u32 code_point)
{
    return !is_ascii_digit(code_point);
}

DigitalFormat digital_format(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    if (auto const& digital_format = locale_data->digital_format(); digital_format.has_value())
        return *digital_format;

    RoundingOptions rounding_options;
    rounding_options.type = RoundingType::SignificantDigits;
    rounding_options.min_significant_digits = 1;
    rounding_options.max_significant_digits = 2;

    auto number_formatter = NumberFormat::create(locale, {}, rounding_options);

    auto icu_locale = adopt_own(*locale_data->locale().clone());
    icu_locale->setUnicodeKeywordValue("nu", "latn", status);
    if (icu_failure(status))
        return {};

    icu::MeasureFormat measurement_formatter(*icu_locale, UMEASFMT_WIDTH_NUMERIC, status);
    if (icu_failure(status))
        return {};

    auto measures = to_array<icu::Measure>({
        { 1, icu::MeasureUnit::createHour(status), status },
        { 22, icu::MeasureUnit::createMinute(status), status },
        { 33, icu::MeasureUnit::createSecond(status), status },
    });
    if (icu_failure(status))
        return {};

    icu::UnicodeString formatted;
    icu::FieldPosition position;
    measurement_formatter.formatMeasures(measures.data(), static_cast<i32>(measures.size()), formatted, position, status);
    if (icu_failure(status))
        return {};

    auto hours_minutes_seconds = icu_string_to_string(formatted);
    GenericLexer lexer { hours_minutes_seconds };

    DigitalFormat digital_format;

    auto hours = lexer.consume_while(is_ascii_digit);
    digital_format.uses_two_digit_hours = hours.length() == 2;

    auto hours_minutes_separator = lexer.consume_while(is_not_ascii_digit);
    digital_format.hours_minutes_separator = MUST(String::from_utf8(hours_minutes_separator));

    lexer.ignore_while(is_ascii_digit);

    auto minutes_seconds_separator = lexer.consume_while(is_not_ascii_digit);
    digital_format.minutes_seconds_separator = MUST(String::from_utf8(minutes_seconds_separator));

    locale_data->set_digital_format(move(digital_format));
    return *locale_data->digital_format();
}

}
