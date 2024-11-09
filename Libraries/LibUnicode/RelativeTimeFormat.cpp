/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/ICU.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>
#include <LibUnicode/PartitionRange.h>
#include <LibUnicode/RelativeTimeFormat.h>

#include <unicode/decimfmt.h>
#include <unicode/numfmt.h>
#include <unicode/reldatefmt.h>

namespace Unicode {

Optional<TimeUnit> time_unit_from_string(StringView time_unit)
{
    if (time_unit == "second"sv)
        return TimeUnit::Second;
    if (time_unit == "minute"sv)
        return TimeUnit::Minute;
    if (time_unit == "hour"sv)
        return TimeUnit::Hour;
    if (time_unit == "day"sv)
        return TimeUnit::Day;
    if (time_unit == "week"sv)
        return TimeUnit::Week;
    if (time_unit == "month"sv)
        return TimeUnit::Month;
    if (time_unit == "quarter"sv)
        return TimeUnit::Quarter;
    if (time_unit == "year"sv)
        return TimeUnit::Year;
    return {};
}

StringView time_unit_to_string(TimeUnit time_unit)
{
    switch (time_unit) {
    case TimeUnit::Second:
        return "second"sv;
    case TimeUnit::Minute:
        return "minute"sv;
    case TimeUnit::Hour:
        return "hour"sv;
    case TimeUnit::Day:
        return "day"sv;
    case TimeUnit::Week:
        return "week"sv;
    case TimeUnit::Month:
        return "month"sv;
    case TimeUnit::Quarter:
        return "quarter"sv;
    case TimeUnit::Year:
        return "year"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr URelativeDateTimeUnit icu_time_unit(TimeUnit unit)
{
    switch (unit) {
    case TimeUnit::Second:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_SECOND;
    case TimeUnit::Minute:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_MINUTE;
    case TimeUnit::Hour:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_HOUR;
    case TimeUnit::Day:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_DAY;
    case TimeUnit::Week:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_WEEK;
    case TimeUnit::Month:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_MONTH;
    case TimeUnit::Quarter:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_QUARTER;
    case TimeUnit::Year:
        return URelativeDateTimeUnit::UDAT_REL_UNIT_YEAR;
    }
    VERIFY_NOT_REACHED();
}

NumericDisplay numeric_display_from_string(StringView numeric_display)
{
    if (numeric_display == "always"sv)
        return NumericDisplay::Always;
    if (numeric_display == "auto"sv)
        return NumericDisplay::Auto;
    VERIFY_NOT_REACHED();
}

StringView numeric_display_to_string(NumericDisplay numeric_display)
{
    switch (numeric_display) {
    case NumericDisplay::Always:
        return "always"sv;
    case NumericDisplay::Auto:
        return "auto"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UDateRelativeDateTimeFormatterStyle icu_relative_date_time_style(Style unit_display)
{
    switch (unit_display) {
    case Style::Long:
        return UDAT_STYLE_LONG;
    case Style::Short:
        return UDAT_STYLE_SHORT;
    case Style::Narrow:
        return UDAT_STYLE_NARROW;
    }
    VERIFY_NOT_REACHED();
}

static constexpr StringView icu_relative_time_format_field_to_string(i32 field)
{
    switch (field) {
    case PartitionRange::LITERAL_FIELD:
        return "literal"sv;
    case UNUM_INTEGER_FIELD:
        return "integer"sv;
    case UNUM_FRACTION_FIELD:
        return "fraction"sv;
    case UNUM_DECIMAL_SEPARATOR_FIELD:
        return "decimal"sv;
    case UNUM_GROUPING_SEPARATOR_FIELD:
        return "group"sv;
    }
    VERIFY_NOT_REACHED();
}

class RelativeTimeFormatImpl : public RelativeTimeFormat {
public:
    explicit RelativeTimeFormatImpl(NonnullOwnPtr<icu::RelativeDateTimeFormatter> formatter)
        : m_formatter(move(formatter))
    {
    }

    virtual ~RelativeTimeFormatImpl() override = default;

    virtual String format(double time, TimeUnit unit, NumericDisplay numeric_display) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_impl(time, unit, numeric_display);

        auto formatted_time = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        return icu_string_to_string(formatted_time);
    }

    virtual Vector<Partition> format_to_parts(double time, TimeUnit unit, NumericDisplay numeric_display) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_impl(time, unit, numeric_display);
        auto unit_string = time_unit_to_string(unit);

        auto formatted_time = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        Vector<Partition> result;
        Vector<PartitionRange> separators;

        auto create_partition = [&](i32 field, i32 begin, i32 end, bool is_unit) {
            Partition partition;
            partition.type = icu_relative_time_format_field_to_string(field);
            partition.value = icu_string_to_string(formatted_time.tempSubStringBetween(begin, end));
            if (is_unit)
                partition.unit = unit_string;
            result.append(move(partition));
        };

        icu::ConstrainedFieldPosition position;
        position.constrainCategory(UFIELD_CATEGORY_NUMBER);

        i32 previous_end_index = 0;

        while (static_cast<bool>(formatted->nextPosition(position, status)) && icu_success(status)) {
            if (position.getField() == UNUM_GROUPING_SEPARATOR_FIELD) {
                separators.empend(position.getField(), position.getStart(), position.getLimit());
                continue;
            }

            if (previous_end_index < position.getStart())
                create_partition(PartitionRange::LITERAL_FIELD, previous_end_index, position.getStart(), false);

            auto start = position.getStart();

            if (position.getField() == UNUM_INTEGER_FIELD) {
                for (auto const& separator : separators) {
                    if (start >= separator.start)
                        continue;

                    create_partition(position.getField(), start, separator.start, true);
                    create_partition(separator.field, separator.start, separator.end, true);

                    start = separator.end;
                    break;
                }
            }

            create_partition(position.getField(), start, position.getLimit(), true);
            previous_end_index = position.getLimit();
        }

        if (previous_end_index < formatted_time.length())
            create_partition(PartitionRange::LITERAL_FIELD, previous_end_index, formatted_time.length(), false);

        return result;
    }

private:
    Optional<icu::FormattedRelativeDateTime> format_impl(double time, TimeUnit unit, NumericDisplay numeric_display) const
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = numeric_display == NumericDisplay::Always
            ? m_formatter->formatNumericToValue(time, icu_time_unit(unit), status)
            : m_formatter->formatToValue(time, icu_time_unit(unit), status);
        if (icu_failure(status))
            return {};

        return formatted;
    }

    NonnullOwnPtr<icu::RelativeDateTimeFormatter> m_formatter;
};

NonnullOwnPtr<RelativeTimeFormat> RelativeTimeFormat::create(StringView locale, Style style)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    auto* number_formatter = icu::NumberFormat::createInstance(locale_data->locale(), UNUM_DECIMAL, status);
    VERIFY(locale_data.has_value());

    if (number_formatter->getDynamicClassID() == icu::DecimalFormat::getStaticClassID())
        static_cast<icu::DecimalFormat&>(*number_formatter).setMinimumGroupingDigits(UNUM_MINIMUM_GROUPING_DIGITS_AUTO);

    auto formatter = make<icu::RelativeDateTimeFormatter>(locale_data->locale(), number_formatter, icu_relative_date_time_style(style), UDISPCTX_CAPITALIZATION_NONE, status);
    VERIFY(icu_success(status));

    return make<RelativeTimeFormatImpl>(move(formatter));
}

}
