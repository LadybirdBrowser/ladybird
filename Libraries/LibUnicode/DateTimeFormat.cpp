/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Array.h>
#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>
#include <AK/TypeCasts.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>
#include <LibUnicode/PartitionRange.h>
#include <stdlib.h>

#include <unicode/calendar.h>
#include <unicode/datefmt.h>
#include <unicode/dtitvfmt.h>
#include <unicode/dtptngen.h>
#include <unicode/gregocal.h>
#include <unicode/smpdtfmt.h>
#include <unicode/timezone.h>
#include <unicode/ucal.h>

namespace Unicode {

DateTimeStyle date_time_style_from_string(StringView style)
{
    if (style == "full"sv)
        return DateTimeStyle::Full;
    if (style == "long"sv)
        return DateTimeStyle::Long;
    if (style == "medium"sv)
        return DateTimeStyle::Medium;
    if (style == "short"sv)
        return DateTimeStyle::Short;
    VERIFY_NOT_REACHED();
}

StringView date_time_style_to_string(DateTimeStyle style)
{
    switch (style) {
    case DateTimeStyle::Full:
        return "full"sv;
    case DateTimeStyle::Long:
        return "long"sv;
    case DateTimeStyle::Medium:
        return "medium"sv;
    case DateTimeStyle::Short:
        return "short"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr icu::DateFormat::EStyle icu_date_time_style(DateTimeStyle style)
{
    switch (style) {
    case DateTimeStyle::Full:
        return icu::DateFormat::EStyle::kFull;
    case DateTimeStyle::Long:
        return icu::DateFormat::EStyle::kLong;
    case DateTimeStyle::Medium:
        return icu::DateFormat::EStyle::kMedium;
    case DateTimeStyle::Short:
        return icu::DateFormat::EStyle::kShort;
    }
    VERIFY_NOT_REACHED();
}

HourCycle hour_cycle_from_string(StringView hour_cycle)
{
    if (hour_cycle == "h11"sv)
        return HourCycle::H11;
    if (hour_cycle == "h12"sv)
        return HourCycle::H12;
    if (hour_cycle == "h23"sv)
        return HourCycle::H23;
    if (hour_cycle == "h24"sv)
        return HourCycle::H24;
    VERIFY_NOT_REACHED();
}

StringView hour_cycle_to_string(HourCycle hour_cycle)
{
    switch (hour_cycle) {
    case HourCycle::H11:
        return "h11"sv;
    case HourCycle::H12:
        return "h12"sv;
    case HourCycle::H23:
        return "h23"sv;
    case HourCycle::H24:
        return "h24"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<HourCycle> default_hour_cycle(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto hour_cycle = locale_data->date_time_pattern_generator().getDefaultHourCycle(status);
    if (icu_failure(status))
        return {};

    switch (hour_cycle) {
    case UDAT_HOUR_CYCLE_11:
        return HourCycle::H11;
    case UDAT_HOUR_CYCLE_12:
        return HourCycle::H12;
    case UDAT_HOUR_CYCLE_23:
        return HourCycle::H23;
    case UDAT_HOUR_CYCLE_24:
        return HourCycle::H24;
    }
    VERIFY_NOT_REACHED();
}

static constexpr char icu_hour_cycle(Optional<HourCycle> const& hour_cycle, Optional<bool> const& hour12)
{
    if (hour12.has_value())
        return *hour12 ? 'h' : 'H';
    if (!hour_cycle.has_value())
        return 'j';

    switch (*hour_cycle) {
    case HourCycle::H11:
        return 'K';
    case HourCycle::H12:
        return 'h';
    case HourCycle::H23:
        return 'H';
    case HourCycle::H24:
        return 'k';
    }
    VERIFY_NOT_REACHED();
}

CalendarPatternStyle calendar_pattern_style_from_string(StringView style)
{
    if (style == "narrow"sv)
        return CalendarPatternStyle::Narrow;
    if (style == "short"sv)
        return CalendarPatternStyle::Short;
    if (style == "long"sv)
        return CalendarPatternStyle::Long;
    if (style == "numeric"sv)
        return CalendarPatternStyle::Numeric;
    if (style == "2-digit"sv)
        return CalendarPatternStyle::TwoDigit;
    if (style == "shortOffset"sv)
        return CalendarPatternStyle::ShortOffset;
    if (style == "longOffset"sv)
        return CalendarPatternStyle::LongOffset;
    if (style == "shortGeneric"sv)
        return CalendarPatternStyle::ShortGeneric;
    if (style == "longGeneric"sv)
        return CalendarPatternStyle::LongGeneric;
    VERIFY_NOT_REACHED();
}

StringView calendar_pattern_style_to_string(CalendarPatternStyle style)
{
    switch (style) {
    case CalendarPatternStyle::Narrow:
        return "narrow"sv;
    case CalendarPatternStyle::Short:
        return "short"sv;
    case CalendarPatternStyle::Long:
        return "long"sv;
    case CalendarPatternStyle::Numeric:
        return "numeric"sv;
    case CalendarPatternStyle::TwoDigit:
        return "2-digit"sv;
    case CalendarPatternStyle::ShortOffset:
        return "shortOffset"sv;
    case CalendarPatternStyle::LongOffset:
        return "longOffset"sv;
    case CalendarPatternStyle::ShortGeneric:
        return "shortGeneric"sv;
    case CalendarPatternStyle::LongGeneric:
        return "longGeneric"sv;
    }
    VERIFY_NOT_REACHED();
}

// https://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
String CalendarPattern::to_pattern() const
{
    // What we refer to as Narrow, Short, and Long, TR-35 refers to as Narrow, Abbreviated, and Wide.
    StringBuilder builder;

    if (era.has_value()) {
        switch (*era) {
        case CalendarPatternStyle::Narrow:
            builder.append("GGGGG"sv);
            break;
        case CalendarPatternStyle::Short:
            builder.append("G"sv);
            break;
        case CalendarPatternStyle::Long:
            builder.append("GGGG"sv);
            break;
        default:
            break;
        }
    }
    if (year.has_value()) {
        switch (*year) {
        case CalendarPatternStyle::Numeric:
            builder.append("y"sv);
            break;
        case CalendarPatternStyle::TwoDigit:
            builder.append("yy"sv);
            break;
        default:
            break;
        }
    }
    if (month.has_value()) {
        switch (*month) {
        case CalendarPatternStyle::Numeric:
            builder.append("M"sv);
            break;
        case CalendarPatternStyle::TwoDigit:
            builder.append("MM"sv);
            break;
        case CalendarPatternStyle::Narrow:
            builder.append("MMMMM"sv);
            break;
        case CalendarPatternStyle::Short:
            builder.append("MMM"sv);
            break;
        case CalendarPatternStyle::Long:
            builder.append("MMMM"sv);
            break;
        default:
            break;
        }
    }
    if (weekday.has_value()) {
        switch (*weekday) {
        case CalendarPatternStyle::Narrow:
            builder.append("EEEEE"sv);
            break;
        case CalendarPatternStyle::Short:
            builder.append("E"sv);
            break;
        case CalendarPatternStyle::Long:
            builder.append("EEEE"sv);
            break;
        default:
            break;
        }
    }
    if (day.has_value()) {
        switch (*day) {
        case CalendarPatternStyle::Numeric:
            builder.append("d"sv);
            break;
        case CalendarPatternStyle::TwoDigit:
            builder.append("dd"sv);
            break;
        default:
            break;
        }
    }
    if (day_period.has_value()) {
        switch (*day_period) {
        case CalendarPatternStyle::Narrow:
            builder.append("BBBBB"sv);
            break;
        case CalendarPatternStyle::Short:
            builder.append("B"sv);
            break;
        case CalendarPatternStyle::Long:
            builder.append("BBBB"sv);
            break;
        default:
            break;
        }
    }
    if (hour.has_value()) {
        auto hour_cycle_symbol = icu_hour_cycle(hour_cycle, hour12);

        switch (*hour) {
        case CalendarPatternStyle::Numeric:
            builder.append(hour_cycle_symbol);
            break;
        case CalendarPatternStyle::TwoDigit:
            builder.append_repeated(hour_cycle_symbol, 2);
            break;
        default:
            break;
        }
    }
    if (minute.has_value()) {
        switch (*minute) {
        case CalendarPatternStyle::Numeric:
            builder.append("m"sv);
            break;
        case CalendarPatternStyle::TwoDigit:
            builder.append("mm"sv);
            break;
        default:
            break;
        }
    }
    if (second.has_value()) {
        switch (*second) {
        case CalendarPatternStyle::Numeric:
            builder.append("s"sv);
            break;
        case CalendarPatternStyle::TwoDigit:
            builder.append("ss"sv);
            break;
        default:
            break;
        }
    }
    if (fractional_second_digits.has_value()) {
        for (u8 i = 0; i < *fractional_second_digits; ++i)
            builder.append("S"sv);
    }
    if (time_zone_name.has_value()) {
        switch (*time_zone_name) {
        case CalendarPatternStyle::Short:
            builder.append("z"sv);
            break;
        case CalendarPatternStyle::Long:
            builder.append("zzzz"sv);
            break;
        case CalendarPatternStyle::ShortOffset:
            builder.append("O"sv);
            break;
        case CalendarPatternStyle::LongOffset:
            builder.append("OOOO"sv);
            break;
        case CalendarPatternStyle::ShortGeneric:
            builder.append("v"sv);
            break;
        case CalendarPatternStyle::LongGeneric:
            builder.append("vvvv"sv);
            break;
        default:
            break;
        }
    }

    return MUST(builder.to_string());
}

// https://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
CalendarPattern CalendarPattern::create_from_pattern(StringView pattern)
{
    GenericLexer lexer { pattern };
    CalendarPattern format {};

    while (!lexer.is_eof()) {
        if (lexer.next_is(is_quote)) {
            lexer.consume_quoted_string();
            continue;
        }

        auto starting_char = lexer.peek();
        auto segment = lexer.consume_while([&](char ch) { return ch == starting_char; });

        // Era
        if (all_of(segment, is_any_of("G"sv))) {
            if (segment.length() <= 3)
                format.era = CalendarPatternStyle::Short;
            else if (segment.length() == 4)
                format.era = CalendarPatternStyle::Long;
            else
                format.era = CalendarPatternStyle::Narrow;
        }

        // Year
        else if (all_of(segment, is_any_of("yYuUr"sv))) {
            if (segment.length() == 2)
                format.year = CalendarPatternStyle::TwoDigit;
            else
                format.year = CalendarPatternStyle::Numeric;
        }

        // Month
        else if (all_of(segment, is_any_of("ML"sv))) {
            if (segment.length() == 1)
                format.month = CalendarPatternStyle::Numeric;
            else if (segment.length() == 2)
                format.month = CalendarPatternStyle::TwoDigit;
            else if (segment.length() == 3)
                format.month = CalendarPatternStyle::Short;
            else if (segment.length() == 4)
                format.month = CalendarPatternStyle::Long;
            else if (segment.length() == 5)
                format.month = CalendarPatternStyle::Narrow;
        }

        // Weekday
        else if (all_of(segment, is_any_of("ecE"sv))) {
            if (segment.length() == 4)
                format.weekday = CalendarPatternStyle::Long;
            else if (segment.length() == 5)
                format.weekday = CalendarPatternStyle::Narrow;
            else
                format.weekday = CalendarPatternStyle::Short;
        }

        // Day
        else if (all_of(segment, is_any_of("d"sv))) {
            if (segment.length() == 1)
                format.day = CalendarPatternStyle::Numeric;
            else
                format.day = CalendarPatternStyle::TwoDigit;
        } else if (all_of(segment, is_any_of("DFg"sv))) {
            format.day = CalendarPatternStyle::Numeric;
        }

        // Day period
        else if (all_of(segment, is_any_of("B"sv))) {
            if (segment.length() == 4)
                format.day_period = CalendarPatternStyle::Long;
            else if (segment.length() == 5)
                format.day_period = CalendarPatternStyle::Narrow;
            else
                format.day_period = CalendarPatternStyle::Short;
        }

        // Hour
        else if (all_of(segment, is_any_of("hHKk"sv))) {
            switch (starting_char) {
            case 'K':
                format.hour_cycle = HourCycle::H11;
                break;
            case 'h':
                format.hour_cycle = HourCycle::H12;
                break;
            case 'H':
                format.hour_cycle = HourCycle::H23;
                break;
            case 'k':
                format.hour_cycle = HourCycle::H24;
                break;
            }

            if (segment.length() == 1)
                format.hour = CalendarPatternStyle::Numeric;
            else
                format.hour = CalendarPatternStyle::TwoDigit;
        }

        // Minute
        else if (all_of(segment, is_any_of("m"sv))) {
            if (segment.length() == 1)
                format.minute = CalendarPatternStyle::Numeric;
            else
                format.minute = CalendarPatternStyle::TwoDigit;
        }

        // Second
        else if (all_of(segment, is_any_of("s"sv))) {
            if (segment.length() == 1)
                format.second = CalendarPatternStyle::Numeric;
            else
                format.second = CalendarPatternStyle::TwoDigit;
        } else if (all_of(segment, is_any_of("S"sv))) {
            format.fractional_second_digits = static_cast<u8>(segment.length());
        }

        // Zone
        else if (all_of(segment, is_any_of("zV"sv))) {
            if (segment.length() < 4)
                format.time_zone_name = CalendarPatternStyle::Short;
            else
                format.time_zone_name = CalendarPatternStyle::Long;
        } else if (all_of(segment, is_any_of("ZOXx"sv))) {
            if (segment.length() < 4)
                format.time_zone_name = CalendarPatternStyle::ShortOffset;
            else
                format.time_zone_name = CalendarPatternStyle::LongOffset;
        } else if (all_of(segment, is_any_of("v"sv))) {
            if (segment.length() < 4)
                format.time_zone_name = CalendarPatternStyle::ShortGeneric;
            else
                format.time_zone_name = CalendarPatternStyle::LongGeneric;
        }
    }

    return format;
}

template<typename T, typename GetRegionalValues>
static T find_regional_values_for_locale(StringView locale, GetRegionalValues&& get_regional_values)
{
    auto has_value = [](auto const& container) {
        if constexpr (requires { container.has_value(); })
            return container.has_value();
        else
            return !container.is_empty();
    };

    if (auto regional_values = get_regional_values(locale); has_value(regional_values))
        return regional_values;

    auto return_default_values = [&]() { return get_regional_values("001"sv); };

    auto language = parse_unicode_language_id(locale);
    if (!language.has_value())
        return return_default_values();

    if (!language->region.has_value()) {
        if (auto maximized = add_likely_subtags(language->to_string()); maximized.has_value())
            language = parse_unicode_language_id(*maximized);
    }

    if (!language.has_value() || !language->region.has_value())
        return return_default_values();

    if (auto regional_values = get_regional_values(*language->region); has_value(regional_values))
        return regional_values;

    return return_default_values();
}

// ICU does not contain a field enumeration for "literal" partitions. Define a custom field so that we may provide a
// type for those partitions.
static constexpr i32 LITERAL_FIELD = -1;

static constexpr StringView icu_date_time_format_field_to_string(i32 field)
{
    switch (field) {
    case LITERAL_FIELD:
        return "literal"sv;
    case UDAT_ERA_FIELD:
        return "era"sv;
    case UDAT_YEAR_FIELD:
    case UDAT_EXTENDED_YEAR_FIELD:
        return "year"sv;
    case UDAT_YEAR_NAME_FIELD:
        return "yearName"sv;
    case UDAT_RELATED_YEAR_FIELD:
        return "relatedYear"sv;
    case UDAT_MONTH_FIELD:
    case UDAT_STANDALONE_MONTH_FIELD:
        return "month"sv;
    case UDAT_DAY_OF_WEEK_FIELD:
    case UDAT_DOW_LOCAL_FIELD:
    case UDAT_STANDALONE_DAY_FIELD:
        return "weekday"sv;
    case UDAT_DATE_FIELD:
        return "day"sv;
    case UDAT_AM_PM_FIELD:
    case UDAT_AM_PM_MIDNIGHT_NOON_FIELD:
    case UDAT_FLEXIBLE_DAY_PERIOD_FIELD:
        return "dayPeriod"sv;
    case UDAT_HOUR_OF_DAY1_FIELD:
    case UDAT_HOUR_OF_DAY0_FIELD:
    case UDAT_HOUR1_FIELD:
    case UDAT_HOUR0_FIELD:
        return "hour"sv;
    case UDAT_MINUTE_FIELD:
        return "minute"sv;
    case UDAT_SECOND_FIELD:
        return "second"sv;
    case UDAT_FRACTIONAL_SECOND_FIELD:
        return "fractionalSecond"sv;
    case UDAT_TIMEZONE_FIELD:
    case UDAT_TIMEZONE_RFC_FIELD:
    case UDAT_TIMEZONE_GENERIC_FIELD:
    case UDAT_TIMEZONE_SPECIAL_FIELD:
    case UDAT_TIMEZONE_LOCALIZED_GMT_OFFSET_FIELD:
    case UDAT_TIMEZONE_ISO_FIELD:
    case UDAT_TIMEZONE_ISO_LOCAL_FIELD:
        return "timeZoneName"sv;
    default:
        return "unknown"sv;
    }
}

static bool apply_hour_cycle_to_skeleton(icu::UnicodeString& skeleton, Optional<HourCycle> const& hour_cycle, Optional<bool> const& hour12)
{
    auto hour_cycle_symbol = icu_hour_cycle(hour_cycle, hour12);
    if (hour_cycle_symbol == 'j')
        return false;

    bool changed_hour_cycle = false;
    bool inside_quote = false;

    for (i32 i = 0; i < skeleton.length(); ++i) {
        switch (skeleton[i]) {
        case '\'':
            inside_quote = !inside_quote;
            break;

        case 'h':
        case 'H':
        case 'k':
        case 'K':
            if (!inside_quote && static_cast<char>(skeleton[i]) != hour_cycle_symbol) {
                skeleton.setCharAt(i, hour_cycle_symbol);
                changed_hour_cycle = true;
            }
            break;

        default:
            break;
        }
    }

    return changed_hour_cycle;
}

static void apply_time_zone_to_formatter(icu::SimpleDateFormat& formatter, icu::Locale const& locale, StringView time_zone_identifier)
{
    UErrorCode status = U_ZERO_ERROR;

    auto time_zone_data = TimeZoneData::for_time_zone(time_zone_identifier);

    auto* calendar = icu::Calendar::createInstance(time_zone_data->time_zone(), locale, status);
    VERIFY(icu_success(status));

    if (calendar->getDynamicClassID() == icu::GregorianCalendar::getStaticClassID()) {
        // https://tc39.es/ecma262/#sec-time-values-and-time-range
        // A time value supports a slightly smaller range of -8,640,000,000,000,000 to 8,640,000,000,000,000 milliseconds.
        static constexpr double ECMA_262_MINIMUM_TIME = -8.64E15;

        auto* gregorian_calendar = static_cast<icu::GregorianCalendar*>(calendar);
        gregorian_calendar->setGregorianChange(ECMA_262_MINIMUM_TIME, status);
        VERIFY(icu_success(status));
    }

    formatter.adoptCalendar(calendar);
}

static bool is_formatted_range_actually_a_range(icu::FormattedDateInterval const& formatted)
{
    UErrorCode status = U_ZERO_ERROR;

    auto result = formatted.toTempString(status);
    if (icu_failure(status))
        return false;

    icu::ConstrainedFieldPosition position;
    position.constrainCategory(UFIELD_CATEGORY_DATE_INTERVAL_SPAN);

    auto has_range = static_cast<bool>(formatted.nextPosition(position, status));
    if (icu_failure(status))
        return false;

    return has_range;
}

class DateTimeFormatImpl : public DateTimeFormat {
public:
    DateTimeFormatImpl(icu::Locale& locale, icu::UnicodeString const& pattern, StringView time_zone_identifier, NonnullOwnPtr<icu::SimpleDateFormat> formatter)
        : m_locale(locale)
        , m_pattern(CalendarPattern::create_from_pattern(icu_string_to_string(pattern)))
        , m_formatter(move(formatter))
    {
        apply_time_zone_to_formatter(*m_formatter, m_locale, time_zone_identifier);
    }

    virtual ~DateTimeFormatImpl() override = default;

    virtual CalendarPattern const& chosen_pattern() const override { return m_pattern; }

    virtual String format(double time) const override
    {
        auto formatted_time = format_impl(time);
        if (!formatted_time.has_value())
            return {};

        return icu_string_to_string(*formatted_time);
    }

    virtual Vector<Partition> format_to_parts(double time) const override
    {
        icu::FieldPositionIterator iterator;

        auto formatted_time = format_impl(time, &iterator);
        if (!formatted_time.has_value())
            return {};

        Vector<Partition> result;

        auto create_partition = [&](i32 field, i32 begin, i32 end) {
            Partition partition;
            partition.type = icu_date_time_format_field_to_string(field);
            partition.value = icu_string_to_string(formatted_time->tempSubStringBetween(begin, end));
            partition.source = "shared"sv;
            result.append(move(partition));
        };

        icu::FieldPosition position;
        i32 previous_end_index = 0;

        while (static_cast<bool>(iterator.next(position))) {
            if (previous_end_index < position.getBeginIndex())
                create_partition(LITERAL_FIELD, previous_end_index, position.getBeginIndex());
            if (position.getField() >= 0)
                create_partition(position.getField(), position.getBeginIndex(), position.getEndIndex());

            previous_end_index = position.getEndIndex();
        }

        if (previous_end_index < formatted_time->length())
            create_partition(LITERAL_FIELD, previous_end_index, formatted_time->length());

        return result;
    }

    virtual String format_range(double start, double end) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_range_impl(start, end);
        if (!formatted.has_value())
            return {};

        if (!is_formatted_range_actually_a_range(*formatted))
            return format(start);

        auto formatted_time = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        normalize_spaces(formatted_time);
        return icu_string_to_string(formatted_time);
    }

    virtual Vector<Partition> format_range_to_parts(double start, double end) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_range_impl(start, end);
        if (!formatted.has_value())
            return {};

        if (!is_formatted_range_actually_a_range(*formatted))
            return format_to_parts(start);

        auto formatted_time = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        normalize_spaces(formatted_time);

        icu::ConstrainedFieldPosition position;
        i32 previous_end_index = 0;

        Vector<Partition> result;
        Optional<PartitionRange> start_range;
        Optional<PartitionRange> end_range;

        auto create_partition = [&](i32 field, i32 begin, i32 end) {
            Partition partition;
            partition.type = icu_date_time_format_field_to_string(field);
            partition.value = icu_string_to_string(formatted_time.tempSubStringBetween(begin, end));

            if (start_range.has_value() && start_range->contains(begin))
                partition.source = "startRange"sv;
            else if (end_range.has_value() && end_range->contains(begin))
                partition.source = "endRange"sv;
            else
                partition.source = "shared"sv;

            result.append(move(partition));
        };

        while (static_cast<bool>(formatted->nextPosition(position, status)) && icu_success(status)) {
            if (previous_end_index < position.getStart())
                create_partition(LITERAL_FIELD, previous_end_index, position.getStart());

            if (position.getCategory() == UFIELD_CATEGORY_DATE_INTERVAL_SPAN) {
                auto& range = position.getField() == 0 ? start_range : end_range;
                range = PartitionRange { position.getField(), position.getStart(), position.getLimit() };
            } else if (position.getCategory() == UFIELD_CATEGORY_DATE) {
                create_partition(position.getField(), position.getStart(), position.getLimit());
            }

            previous_end_index = position.getLimit();
        }

        if (previous_end_index < formatted_time.length())
            create_partition(LITERAL_FIELD, previous_end_index, formatted_time.length());

        return result;
    }

private:
    Optional<icu::UnicodeString> format_impl(double time, icu::FieldPositionIterator* iterator = nullptr) const
    {
        UErrorCode status = U_ZERO_ERROR;
        icu::UnicodeString formatted_time;

        m_formatter->format(time, formatted_time, iterator, status);
        if (icu_failure(status))
            return {};

        normalize_spaces(formatted_time);
        return formatted_time;
    }

    Optional<icu::FormattedDateInterval> format_range_impl(double start, double end) const
    {
        UErrorCode status = U_ZERO_ERROR;

        if (!m_range_formatter) {
            icu::UnicodeString pattern;
            m_formatter->toPattern(pattern);

            auto skeleton = icu::DateTimePatternGenerator::staticGetSkeleton(pattern, status);
            if (icu_failure(status))
                return {};

            auto* formatter = icu::DateIntervalFormat::createInstance(skeleton, m_locale, status);
            if (icu_failure(status))
                return {};

            m_range_formatter = adopt_own(*formatter);
            m_range_formatter->setTimeZone(m_formatter->getTimeZone());
        }

        auto start_calendar = adopt_own(*m_formatter->getCalendar()->clone());
        start_calendar->setTime(start, status);
        if (icu_failure(status))
            return {};

        auto end_calendar = adopt_own(*m_formatter->getCalendar()->clone());
        end_calendar->setTime(end, status);
        if (icu_failure(status))
            return {};

        auto formatted = m_range_formatter->formatToValue(*start_calendar, *end_calendar, status);
        if (icu_failure(status))
            return {};

        return formatted;
    }

    // ICU 72 introduced the use of NBSP to separate time fields and day periods. All major browsers have found that
    // this significantly breaks web compatibilty, and they all replace these spaces with normal ASCII spaces. See:
    //
    // https://bugzilla.mozilla.org/show_bug.cgi?id=1806042
    // https://bugs.webkit.org/show_bug.cgi?id=252147
    // https://issues.chromium.org/issues/40256057
    static void normalize_spaces(icu::UnicodeString& string)
    {
        static char16_t NARROW_NO_BREAK_SPACE = 0x202f;
        static char16_t THIN_SPACE = 0x2009;

        for (i32 i = 0; i < string.length(); ++i) {
            if (string[i] == NARROW_NO_BREAK_SPACE || string[i] == THIN_SPACE)
                string.setCharAt(i, ' ');
        }
    }

    icu::Locale& m_locale;
    CalendarPattern m_pattern;

    NonnullOwnPtr<icu::SimpleDateFormat> m_formatter;
    mutable OwnPtr<icu::DateIntervalFormat> m_range_formatter;
};

NonnullOwnPtr<DateTimeFormat> DateTimeFormat::create_for_date_and_time_style(
    StringView locale,
    StringView time_zone_identifier,
    Optional<HourCycle> const& hour_cycle,
    Optional<bool> const& hour12,
    Optional<DateTimeStyle> const& date_style,
    Optional<DateTimeStyle> const& time_style)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    auto formatter = adopt_own(*as<icu::SimpleDateFormat>([&]() {
        if (date_style.has_value() && time_style.has_value()) {
            return icu::DateFormat::createDateTimeInstance(
                icu_date_time_style(*date_style), icu_date_time_style(*time_style), locale_data->locale());
        }
        if (date_style.has_value()) {
            return icu::DateFormat::createDateInstance(
                icu_date_time_style(*date_style), locale_data->locale());
        }
        if (time_style.has_value()) {
            return icu::DateFormat::createTimeInstance(
                icu_date_time_style(*time_style), locale_data->locale());
        }
        VERIFY_NOT_REACHED();
    }()));

    icu::UnicodeString pattern;
    formatter->toPattern(pattern);

    auto skeleton = icu::DateTimePatternGenerator::staticGetSkeleton(pattern, status);
    VERIFY(icu_success(status));

    if (apply_hour_cycle_to_skeleton(skeleton, hour_cycle, hour12)) {
        pattern = locale_data->date_time_pattern_generator().getBestPattern(skeleton, UDATPG_MATCH_ALL_FIELDS_LENGTH, status);
        VERIFY(icu_success(status));

        apply_hour_cycle_to_skeleton(pattern, hour_cycle, hour12);

        formatter = adopt_own(*new icu::SimpleDateFormat(pattern, locale_data->locale(), status));
        VERIFY(icu_success(status));
    }

    return adopt_own(*new DateTimeFormatImpl(locale_data->locale(), pattern, time_zone_identifier, move(formatter)));
}

NonnullOwnPtr<DateTimeFormat> DateTimeFormat::create_for_pattern_options(
    StringView locale,
    StringView time_zone_identifier,
    CalendarPattern const& options)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    auto skeleton = icu_string(options.to_pattern());
    auto pattern = locale_data->date_time_pattern_generator().getBestPattern(skeleton, UDATPG_MATCH_ALL_FIELDS_LENGTH, status);
    VERIFY(icu_success(status));

    apply_hour_cycle_to_skeleton(pattern, options.hour_cycle, {});

    auto formatter = adopt_own(*new icu::SimpleDateFormat(pattern, locale_data->locale(), status));
    VERIFY(icu_success(status));

    return adopt_own(*new DateTimeFormatImpl(locale_data->locale(), pattern, time_zone_identifier, move(formatter)));
}

static constexpr Weekday icu_calendar_day_to_weekday(UCalendarDaysOfWeek day)
{
    switch (day) {
    case UCAL_SUNDAY:
        return Weekday::Sunday;
    case UCAL_MONDAY:
        return Weekday::Monday;
    case UCAL_TUESDAY:
        return Weekday::Tuesday;
    case UCAL_WEDNESDAY:
        return Weekday::Wednesday;
    case UCAL_THURSDAY:
        return Weekday::Thursday;
    case UCAL_FRIDAY:
        return Weekday::Friday;
    case UCAL_SATURDAY:
        return Weekday::Saturday;
    }
    VERIFY_NOT_REACHED();
}

WeekInfo week_info_of_locale(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto calendar = adopt_own_if_nonnull(icu::Calendar::createInstance(locale_data->locale(), status));
    if (icu_failure(status))
        return {};

    WeekInfo week_info;
    week_info.minimal_days_in_first_week = calendar->getMinimalDaysInFirstWeek();

    if (auto day = calendar->getFirstDayOfWeek(status); icu_success(status))
        week_info.first_day_of_week = icu_calendar_day_to_weekday(day);

    auto append_if_weekend = [&](auto day) {
        auto type = calendar->getDayOfWeekType(day, status);
        if (icu_failure(status))
            return;

        switch (type) {
        case UCAL_WEEKEND_ONSET:
        case UCAL_WEEKEND_CEASE:
        case UCAL_WEEKEND:
            week_info.weekend_days.append(icu_calendar_day_to_weekday(day));
            break;
        default:
            break;
        }
    };

    append_if_weekend(UCAL_SUNDAY);
    append_if_weekend(UCAL_MONDAY);
    append_if_weekend(UCAL_TUESDAY);
    append_if_weekend(UCAL_WEDNESDAY);
    append_if_weekend(UCAL_THURSDAY);
    append_if_weekend(UCAL_FRIDAY);
    append_if_weekend(UCAL_SATURDAY);

    return week_info;
}

}
