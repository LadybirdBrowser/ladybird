/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <LibJS/Runtime/Temporal/DateEquations.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Temporal {

enum class Extended {
    No,
    Yes,
};

enum class Separator {
    No,
    Yes,
};

enum class TimeRequired {
    No,
    Yes,
};

enum class ZDesignator {
    No,
    Yes,
};

enum class Zoned {
    No,
    Yes,
};

// 13.30.1 Static Semantics: IsValidMonthDay, https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-isvalidmonthday
static bool is_valid_month_day(ParseResult const& result)
{
    // 1. If DateDay is "31" and DateMonth is "02", "04", "06", "09", "11", return false.
    if (result.date_day == "31"sv && result.date_month->is_one_of("02"sv, "04"sv, "06"sv, "09"sv, "11"sv))
        return false;

    // 2. If DateMonth is "02" and DateDay is "30", return false.
    if (result.date_month == "02"sv && result.date_day == "30"sv)
        return false;

    // 3. Return true.
    return true;
}

// 13.30.2 Static Semantics: IsValidDate, https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar-static-semantics-isvaliddate
static bool is_valid_date(ParseResult const& result)
{
    // 1. If IsValidMonthDay of DateSpec is false, return false.
    if (!is_valid_month_day(result))
        return false;

    // 2. Let year be ℝ(StringToNumber(CodePointsToString(DateYear))).
    auto year = string_to_number(*result.date_year);

    // 3. If DateMonth is "02" and DateDay is "29" and MathematicalInLeapYear(EpochTimeForYear(year)) = 0, return false.
    if (result.date_month == "02"sv && result.date_day == "29"sv && mathematical_in_leap_year(epoch_time_for_year(year)) == 0)
        return false;

    // 4. Return true.
    return true;
}

// 13.30 ISO 8601 grammar, https://tc39.es/proposal-temporal/#sec-temporal-iso8601grammar
class ISO8601Parser {
public:
    explicit ISO8601Parser(StringView input)
        : m_input(input)
        , m_state({
              .lexer = GenericLexer { input },
              .parse_result = {},
          })
    {
    }

    [[nodiscard]] GenericLexer const& lexer() const { return m_state.lexer; }
    [[nodiscard]] ParseResult const& parse_result() const { return m_state.parse_result; }

    // https://tc39.es/proposal-temporal/#prod-TemporalDateTimeString
    [[nodiscard]] bool parse_temporal_date_time_string()
    {
        // TemporalDateTimeString[Zoned] :::
        //     AnnotatedDateTime[?Zoned, ~TimeRequired]
        return parse_annotated_date_time(Zoned::No, TimeRequired::No);
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalDateTimeString
    [[nodiscard]] bool parse_temporal_zoned_date_time_string()
    {
        // TemporalDateTimeString[Zoned] :::
        //     AnnotatedDateTime[?Zoned, ~TimeRequired]
        return parse_annotated_date_time(Zoned::Yes, TimeRequired::No);
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalDurationString
    [[nodiscard]] bool parse_temporal_duration_string()
    {
        // TemporalDurationString :::
        //     Duration
        return parse_duration();
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalInstantString
    [[nodiscard]] bool parse_temporal_instant_string()
    {
        // TemporalInstantString :::
        //     Date DateTimeSeparator Time DateTimeUTCOffset[+Z] TimeZoneAnnotation[opt] Annotations[opt]
        if (!parse_date())
            return false;
        if (!parse_date_time_separator())
            return false;
        if (!parse_time())
            return false;
        if (!parse_date_time_utc_offset(ZDesignator::Yes))
            return false;

        (void)parse_time_zone_annotation();
        (void)parse_annotations();

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalMonthDayString
    [[nodiscard]] bool parse_temporal_month_day_string()
    {
        // TemporalMonthDayString :::
        //     AnnotatedMonthDay
        //     AnnotatedDateTime[~Zoned, ~TimeRequired]
        //  NOTE: Reverse order here because `AnnotatedMonthDay` can be a subset of `AnnotatedDateTime`.
        return parse_annotated_date_time(Zoned::No, TimeRequired::No) || parse_annotated_month_day();
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalTimeString
    [[nodiscard]] bool parse_temporal_time_string()
    {
        // TemporalTimeString :::
        //     AnnotatedTime
        //     AnnotatedDateTime[~Zoned, +TimeRequired]
        // NOTE: Reverse order here because `AnnotatedTime` can be a subset of `AnnotatedDateTime`.
        return parse_annotated_date_time(Zoned::No, TimeRequired::Yes) || parse_annotated_time();
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalYearMonthString
    [[nodiscard]] bool parse_temporal_year_month_string()
    {
        // TemporalYearMonthString :::
        //     AnnotatedYearMonth
        //     AnnotatedDateTime[~Zoned, ~TimeRequired]
        //  NOTE: Reverse order here because `AnnotatedYearMonth` can be a subset of `AnnotatedDateTime`.
        return parse_annotated_date_time(Zoned::No, TimeRequired::No) || parse_annotated_year_month();
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedDateTime
    [[nodiscard]] bool parse_annotated_date_time(Zoned zoned, TimeRequired time_required)
    {
        // AnnotatedDateTime[Zoned, TimeRequired] :::
        //     [~Zoned] DateTime[~Z, ?TimeRequired] TimeZoneAnnotation[opt] Annotations[opt]
        //     [+Zoned] DateTime[+Z, ?TimeRequired] TimeZoneAnnotation Annotations[opt]
        if (!parse_date_time(zoned == Zoned::Yes ? ZDesignator::Yes : ZDesignator::No, time_required))
            return false;

        if (!parse_time_zone_annotation()) {
            if (zoned == Zoned::Yes)
                return false;
        }

        (void)parse_annotations();

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedMonthDay
    [[nodiscard]] bool parse_annotated_month_day()
    {
        // AnnotatedMonthDay :::
        //     DateSpecMonthDay TimeZoneAnnotation[opt] Annotations[opt]
        if (!parse_date_spec_month_day())
            return false;

        (void)parse_time_zone_annotation();
        (void)parse_annotations();

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedTime
    [[nodiscard]] bool parse_annotated_time()
    {
        // AnnotatedTime :::
        //     TimeDesignator Time DateTimeUTCOffset[~Z][opt] TimeZoneAnnotation[opt] Annotations[opt]
        //     Time DateTimeUTCOffset[~Z][opt] TimeZoneAnnotation[opt] Annotations[opt]
        (void)parse_time_designator();

        if (!parse_time())
            return false;

        (void)parse_date_time_utc_offset(ZDesignator::No);
        (void)parse_time_zone_annotation();
        (void)parse_annotations();

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotatedYearMonth
    [[nodiscard]] bool parse_annotated_year_month()
    {
        // AnnotatedYearMonth :::
        //     DateSpecYearMonth TimeZoneAnnotation[opt] Annotations[opt]
        if (!parse_date_spec_year_month())
            return false;

        (void)parse_time_zone_annotation();
        (void)parse_annotations();

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateTime
    [[nodiscard]] bool parse_date_time(ZDesignator z_designator, TimeRequired time_required)
    {
        StateTransaction transaction { *this };

        // DateTime[Z, TimeRequired] :::
        //     [~TimeRequired] Date
        //     Date DateTimeSeparator Time DateTimeUTCOffset[?Z][opt]
        if (!parse_date())
            return false;

        if (parse_date_time_separator()) {
            if (!parse_time())
                return false;

            (void)parse_date_time_utc_offset(z_designator);
        } else if (time_required == TimeRequired::Yes) {
            return false;
        }

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-Date
    [[nodiscard]] bool parse_date()
    {
        // Date :::
        //     DateSpec[+Extended]
        //     DateSpec[~Extended]
        return parse_date_spec(Extended::Yes) || parse_date_spec(Extended::No);
    }

    // https://tc39.es/proposal-temporal/#prod-DateSpec
    [[nodiscard]] bool parse_date_spec(Extended extended)
    {
        StateTransaction transaction { *this };

        // DateSpec[Extended] :::
        //     DateYear DateSeparator[?Extended] DateMonth DateSeparator[?Extended] DateDay
        if (!parse_date_year())
            return false;
        if (!parse_date_separator(extended))
            return false;
        if (!parse_date_month())
            return false;
        if (!parse_date_separator(extended))
            return false;
        if (!parse_date_day())
            return false;

        // It is a Syntax Error if IsValidDate of DateSpec is false.
        if (!is_valid_date(m_state.parse_result))
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateSpecMonthDay
    [[nodiscard]] bool parse_date_spec_month_day()
    {
        StateTransaction transaction { *this };

        // DateSpecMonthDay :::
        //     --[opt] DateMonth DateSeparator[+Extended] DateDay
        //     --[opt] DateMonth DateSeparator[~Extended] DateDay
        (void)m_state.lexer.consume_specific("--"sv);

        if (!parse_date_month())
            return false;
        if (!parse_date_separator(Extended::Yes) && !parse_date_separator(Extended::No))
            return false;
        if (!parse_date_day())
            return false;

        // It is a Syntax Error if IsValidMonthDay of DateSpecMonthDay is false.
        if (!is_valid_month_day(m_state.parse_result))
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateSpecYearMonth
    [[nodiscard]] bool parse_date_spec_year_month()
    {
        StateTransaction transaction { *this };

        // DateSpecYearMonth :::
        //     DateYear DateSeparator[+Extended] DateMonth
        //     DateYear DateSeparator[~Extended] DateMonth
        if (!parse_date_year())
            return false;
        if (!parse_date_separator(Extended::Yes) && !parse_date_separator(Extended::No))
            return false;
        if (!parse_date_month())
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateYear
    [[nodiscard]] bool parse_date_year()
    {
        StateTransaction transaction { *this };

        // DateYear :::
        //     DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     ASCIISign DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        size_t digit_count = parse_ascii_sign() ? 6 : 4;

        for (size_t i = 0; i < digit_count; ++i) {
            if (!parse_decimal_digit())
                return false;
        }

        // It is a Syntax Error if DateYear is "-000000".
        if (transaction.parsed_string_view() == "-000000"sv)
            return false;

        m_state.parse_result.date_year = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateMonth
    [[nodiscard]] bool parse_date_month()
    {
        StateTransaction transaction { *this };

        // DateMonth :::
        //     0 NonZeroDigit
        //     10
        //     11
        //     12
        if (m_state.lexer.consume_specific('0')) {
            if (!parse_non_zero_digit())
                return false;
        } else {
            auto success = m_state.lexer.consume_specific("10"sv) || m_state.lexer.consume_specific("11"sv) || m_state.lexer.consume_specific("12"sv);
            if (!success)
                return false;
        }

        m_state.parse_result.date_month = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateDay
    [[nodiscard]] bool parse_date_day()
    {
        StateTransaction transaction { *this };

        // DateDay :::
        //     0 NonZeroDigit
        //     1 DecimalDigit
        //     2 DecimalDigit
        //     30
        //     31
        if (m_state.lexer.consume_specific('0')) {
            if (!parse_non_zero_digit())
                return false;
        } else if (m_state.lexer.consume_specific('1') || m_state.lexer.consume_specific('2')) {
            if (!parse_decimal_digit())
                return false;
        } else {
            auto success = m_state.lexer.consume_specific("30"sv) || m_state.lexer.consume_specific("31"sv);
            if (!success)
                return false;
        }

        m_state.parse_result.date_day = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DateTimeUTCOffset
    [[nodiscard]] bool parse_date_time_utc_offset(ZDesignator z_designator)
    {
        // DateTimeUTCOffset[Z] :::
        //     [+Z] UTCDesignator
        //     UTCOffset[+SubMinutePrecision]
        if (z_designator == ZDesignator::Yes) {
            if (parse_utc_designator())
                return true;
        }

        return parse_utc_offset(SubMinutePrecision::Yes, m_state.parse_result.date_time_offset);
    }

    // https://tc39.es/proposal-temporal/#prod-Time
    [[nodiscard]] bool parse_time()
    {
        // Time :::
        //     TimeSpec[+Extended]
        //     TimeSpec[~Extended]
        return parse_time_spec();
    }

    // https://tc39.es/proposal-temporal/#prod-TimeSpec
    [[nodiscard]] bool parse_time_spec()
    {
        StateTransaction transaction { *this };

        auto parse_time_hour = [&]() {
            return scoped_parse(m_state.parse_result.time_hour, [&]() { return parse_hour(); });
        };
        auto parse_time_minute = [&]() {
            return scoped_parse(m_state.parse_result.time_minute, [&]() { return parse_minute_second(); });
        };
        auto parse_time_fraction = [&]() {
            return scoped_parse(m_state.parse_result.time_fraction, [&]() { return parse_temporal_decimal_fraction(); });
        };

        // TimeSpec[Extended] :::
        //     Hour
        //     Hour TimeSeparator[?Extended] MinuteSecond
        //     Hour TimeSeparator[?Extended] MinuteSecond TimeSeparator[?Extended] TimeSecond TemporalDecimalFraction[opt]
        if (!parse_time_hour())
            return false;

        if (parse_time_separator(Extended::Yes)) {
            if (!parse_time_minute())
                return false;

            if (parse_time_separator(Extended::Yes)) {
                if (!parse_time_second())
                    return false;

                (void)parse_time_fraction();
            }
        } else if (parse_time_minute() && parse_time_second()) {
            (void)parse_time_fraction();
        }

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TimeSecond
    [[nodiscard]] bool parse_time_second()
    {
        StateTransaction transaction { *this };

        // TimeSecond :::
        //     MinuteSecond
        //     60
        auto success = parse_minute_second() || m_state.lexer.consume_specific("60"sv);
        if (!success)
            return false;

        m_state.parse_result.time_second = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneAnnotation
    [[nodiscard]] bool parse_time_zone_annotation()
    {
        StateTransaction transaction { *this };

        // TimeZoneAnnotation :::
        //    [ AnnotationCriticalFlag[opt] TimeZoneIdentifier ]
        if (!m_state.lexer.consume_specific('['))
            return false;

        (void)parse_annotation_critical_flag();
        if (!parse_time_zone_identifier())
            return false;

        if (!m_state.lexer.consume_specific(']'))
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneIdentifier
    [[nodiscard]] bool parse_time_zone_identifier()
    {
        StateTransaction transaction { *this };

        // TimeZoneIdentifier :::
        //    UTCOffset[~SubMinutePrecision]
        //    TimeZoneIANAName
        auto success = parse_utc_offset(SubMinutePrecision::No, m_state.parse_result.time_zone_offset) || parse_time_zone_iana_name();
        if (!success)
            return false;

        m_state.parse_result.time_zone_identifier = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneIANAName
    [[nodiscard]] bool parse_time_zone_iana_name()
    {
        StateTransaction transaction { *this };

        // TimeZoneIANAName :::
        //     TimeZoneIANANameComponent
        //     TimeZoneIANAName / TimeZoneIANANameComponent
        if (!parse_time_zone_iana_name_component())
            return false;

        while (m_state.lexer.consume_specific('/')) {
            if (!parse_time_zone_iana_name_component())
                return false;
        }

        m_state.parse_result.time_zone_iana_name = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TimeZoneIANANameComponent
    [[nodiscard]] bool parse_time_zone_iana_name_component()
    {
        // TimeZoneIANANameComponent :::
        //     TZLeadingChar
        //     TimeZoneIANANameComponent TZChar
        if (!parse_tz_leading_char())
            return false;
        while (parse_tz_leading_char())
            ;
        while (parse_tz_char())
            ;

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TZLeadingChar
    [[nodiscard]] bool parse_tz_leading_char()
    {
        // TZLeadingChar :::
        //     Alpha
        //     .
        //     _
        return parse_alpha() || m_state.lexer.consume_specific('.') || m_state.lexer.consume_specific('_');
    }

    // https://tc39.es/proposal-temporal/#prod-TZChar
    [[nodiscard]] bool parse_tz_char()
    {
        // TZChar :::
        //     TZLeadingChar
        //     DecimalDigit
        //     -
        //     +
        return parse_tz_leading_char() || parse_decimal_digit() || m_state.lexer.consume_specific('.') || m_state.lexer.consume_specific('+');
    }

    // https://tc39.es/proposal-temporal/#prod-Annotations
    [[nodiscard]] bool parse_annotations()
    {
        // Annotations :::
        //     Annotation Annotationsopt
        if (!parse_annotation())
            return false;
        while (parse_annotation())
            ;
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-Annotation
    [[nodiscard]] bool parse_annotation()
    {
        StateTransaction transaction { *this };

        Optional<StringView> key;
        Optional<StringView> value;

        // Annotation :::
        //     [ AnnotationCriticalFlag[opt] AnnotationKey = AnnotationValue ]
        if (!m_state.lexer.consume_specific('['))
            return false;

        auto critical = parse_annotation_critical_flag();

        if (!scoped_parse(key, [&]() { return parse_annotation_key(); }))
            return false;
        if (!m_state.lexer.consume_specific('='))
            return false;
        if (!scoped_parse(value, [&]() { return parse_annotation_value(); }))
            return false;

        if (!m_state.lexer.consume_specific(']'))
            return false;

        m_state.parse_result.annotations.empend(critical, *key, *value);
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationKey
    [[nodiscard]] bool parse_annotation_key()
    {
        // AnnotationKey :::
        //     AKeyLeadingChar
        //     AnnotationKey AKeyChar
        if (!parse_annotation_key_leading_char())
            return false;
        while (parse_annotation_key_leading_char())
            ;
        while (parse_annotation_key_char())
            ;

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AKeyLeadingChar
    [[nodiscard]] bool parse_annotation_key_leading_char()
    {
        // AKeyLeadingChar :::
        //     LowercaseAlpha
        //     _
        return parse_lowercase_alpha() || m_state.lexer.consume_specific('_');
    }

    // https://tc39.es/proposal-temporal/#prod-AKeyChar
    [[nodiscard]] bool parse_annotation_key_char()
    {
        // AKeyChar :::
        //     AKeyLeadingChar
        //     DecimalDigit
        //     -
        return parse_annotation_key_leading_char() || parse_decimal_digit() || m_state.lexer.consume_specific('-');
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationValue
    [[nodiscard]] bool parse_annotation_value()
    {
        // AnnotationValue :::
        //     AnnotationValueComponent
        //     AnnotationValueComponent - AnnotationValue
        if (!parse_annotation_value_component())
            return false;

        while (m_state.lexer.consume_specific('-')) {
            if (!parse_annotation_value_component())
                return false;
        }

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationValueComponent
    [[nodiscard]] bool parse_annotation_value_component()
    {
        // AnnotationValueComponent :::
        //     Alpha AnnotationValueComponent[opt]
        //     DecimalDigit AnnotationValueComponent[opt]
        auto parse_component = [&]() { return parse_alpha() || parse_decimal_digit(); };

        if (!parse_component())
            return false;
        while (parse_component())
            ;

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-UTCOffset
    [[nodiscard]] bool parse_utc_offset(SubMinutePrecision sub_minute_precision, Optional<TimeZoneOffset>& result)
    {
        StateTransaction transaction { *this };
        TimeZoneOffset time_zone_offset;

        auto parse_utc_sign = [&]() {
            return scoped_parse(time_zone_offset.sign, [&]() { return parse_ascii_sign(); });
        };
        auto parse_utc_hours = [&]() {
            return scoped_parse(time_zone_offset.hours, [&]() { return parse_hour(); });
        };
        auto parse_utc_minutes = [&]() {
            return scoped_parse(time_zone_offset.minutes, [&]() { return parse_minute_second(); });
        };
        auto parse_utc_seconds = [&]() {
            return scoped_parse(time_zone_offset.seconds, [&]() { return parse_minute_second(); });
        };
        auto parse_utc_fraction = [&]() {
            return scoped_parse(time_zone_offset.fraction, [&]() { return parse_temporal_decimal_fraction(); });
        };

        // UTCOffset[SubMinutePrecision] :::
        //     ASCIISign Hour
        //     ASCIISign Hour TimeSeparator[+Extended] MinuteSecond
        //     ASCIISign Hour TimeSeparator[~Extended] MinuteSecond
        //     [+SubMinutePrecision] ASCIISign Hour TimeSeparator[+Extended] MinuteSecond TimeSeparator[+Extended] MinuteSecond TemporalDecimalFraction[opt]
        //     [+SubMinutePrecision] ASCIISign Hour TimeSeparator[~Extended] MinuteSecond TimeSeparator[~Extended] MinuteSecond TemporalDecimalFraction[opt]
        if (!parse_utc_sign())
            return false;
        if (!parse_utc_hours())
            return false;

        if (parse_time_separator(Extended::Yes)) {
            if (!parse_utc_minutes())
                return false;

            if (sub_minute_precision == SubMinutePrecision::Yes && parse_time_separator(Extended::Yes)) {
                if (!parse_utc_seconds())
                    return false;

                (void)parse_utc_fraction();
            }
        } else if (parse_utc_minutes()) {
            if (sub_minute_precision == SubMinutePrecision::Yes && parse_utc_seconds())
                (void)parse_utc_fraction();
        }

        time_zone_offset.source_text = transaction.parsed_string_view();
        result = move(time_zone_offset);

        transaction.commit();
        return true;
    }

    // https://tc39.es/ecma262/#prod-Hour
    [[nodiscard]] bool parse_hour()
    {
        // Hour :::
        //     0 DecimalDigit
        //     1 DecimalDigit
        //     20
        //     21
        //     22
        //     23
        if (m_state.lexer.consume_specific('0') || m_state.lexer.consume_specific('1')) {
            if (!parse_decimal_digit())
                return false;
        } else {
            auto success = m_state.lexer.consume_specific("20"sv)
                || m_state.lexer.consume_specific("21"sv)
                || m_state.lexer.consume_specific("22"sv)
                || m_state.lexer.consume_specific("23"sv);
            if (!success)
                return false;
        }

        return true;
    }

    // https://tc39.es/ecma262/#prod-MinuteSecond
    [[nodiscard]] bool parse_minute_second()
    {
        // MinuteSecond :::
        //     0 DecimalDigit
        //     1 DecimalDigit
        //     2 DecimalDigit
        //     3 DecimalDigit
        //     4 DecimalDigit
        //     5 DecimalDigit
        auto success = m_state.lexer.consume_specific('0')
            || m_state.lexer.consume_specific('1')
            || m_state.lexer.consume_specific('2')
            || m_state.lexer.consume_specific('3')
            || m_state.lexer.consume_specific('4')
            || m_state.lexer.consume_specific('5');
        if (!success)
            return false;
        if (!parse_decimal_digit())
            return false;

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDate
    [[nodiscard]] bool parse_duration_date()
    {
        // DurationDate :::
        //     DurationYearsPart DurationTime[opt]
        //     DurationMonthsPart DurationTime[opt]
        //     DurationWeeksPart DurationTime[opt]
        //     DurationDaysPart DurationTime[opt]
        auto success = parse_duration_years_part() || parse_duration_months_part() || parse_duration_weeks_part() || parse_duration_days_part();
        if (!success)
            return false;

        (void)parse_duration_time();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-Duration
    [[nodiscard]] bool parse_duration()
    {
        StateTransaction transaction { *this };

        // Duration :::
        //    ASCIISign[opt] DurationDesignator DurationDate
        //    ASCIISign[opt] DurationDesignator DurationTime
        (void)scoped_parse(m_state.parse_result.sign, [&]() { return parse_ascii_sign(); });

        if (!parse_duration_designator())
            return false;

        auto success = parse_duration_date() || parse_duration_time();
        if (!success)
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationYearsPart
    [[nodiscard]] bool parse_duration_years_part()
    {
        StateTransaction transaction { *this };

        // DurationYearsPart :::
        //     DecimalDigits[~Sep] YearsDesignator DurationMonthsPart
        //     DecimalDigits[~Sep] YearsDesignator DurationWeeksPart
        //     DecimalDigits[~Sep] YearsDesignator DurationDaysPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_years))
            return false;

        if (!parse_years_designator())
            return false;

        (void)(parse_duration_months_part() || parse_duration_weeks_part() || parse_duration_days_part());

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationMonthsPart
    [[nodiscard]] bool parse_duration_months_part()
    {
        StateTransaction transaction { *this };

        // DurationMonthsPart :::
        //     DecimalDigits[~Sep] MonthsDesignator DurationWeeksPart
        //     DecimalDigits[~Sep] MonthsDesignator DurationDaysPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_months))
            return false;

        if (!parse_months_designator())
            return false;

        (void)(parse_duration_weeks_part() || parse_duration_days_part());

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationWeeksPart
    [[nodiscard]] bool parse_duration_weeks_part()
    {
        StateTransaction transaction { *this };

        // DurationWeeksPart :::
        //     DecimalDigits[~Sep] WeeksDesignator DurationDaysPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_weeks))
            return false;

        if (!parse_weeks_designator())
            return false;

        (void)parse_duration_days_part();

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDaysPart
    [[nodiscard]] bool parse_duration_days_part()
    {
        StateTransaction transaction { *this };

        // DurationDaysPart :::
        //     DecimalDigits[~Sep] DaysDesignator
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_days))
            return false;

        if (!parse_days_designator())
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationTime
    [[nodiscard]] bool parse_duration_time()
    {
        StateTransaction transaction { *this };

        // DurationTime :::
        //     TimeDesignator DurationHoursPart
        //     TimeDesignator DurationMinutesPart
        //     TimeDesignator DurationSecondsPart
        if (!parse_time_designator())
            return false;

        auto success = parse_duration_hours_part() || parse_duration_minutes_part() || parse_duration_seconds_part();
        if (!success)
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationHoursPart
    [[nodiscard]] bool parse_duration_hours_part()
    {
        StateTransaction transaction { *this };

        // DurationHoursPart :::
        //     DecimalDigits[~Sep] TemporalDecimalFraction HoursDesignator
        //     DecimalDigits[~Sep] HoursDesignator DurationMinutesPart
        //     DecimalDigits[~Sep] HoursDesignator DurationSecondsPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_hours))
            return false;

        auto is_fractional = scoped_parse(m_state.parse_result.duration_hours_fraction, [&]() { return parse_temporal_decimal_fraction(); });

        if (!parse_hours_designator())
            return false;
        if (!is_fractional)
            (void)(parse_duration_minutes_part() || parse_duration_seconds_part());

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationMinutesPart
    [[nodiscard]] bool parse_duration_minutes_part()
    {
        StateTransaction transaction { *this };

        // DurationMinutesPart :::
        //     DecimalDigits[~Sep] TemporalDecimalFraction MinutesDesignator
        //     DecimalDigits[~Sep] MinutesDesignator DurationSecondsPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_minutes))
            return false;

        auto is_fractional = scoped_parse(m_state.parse_result.duration_minutes_fraction, [&]() { return parse_temporal_decimal_fraction(); });

        if (!parse_minutes_designator())
            return false;
        if (!is_fractional)
            (void)parse_duration_seconds_part();

        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-DurationSecondsPart
    [[nodiscard]] bool parse_duration_seconds_part()
    {
        StateTransaction transaction { *this };

        // DurationSecondsPart :::
        //     DecimalDigits[~Sep] TemporalDecimalFraction[opt] SecondsDesignator
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_seconds))
            return false;

        (void)scoped_parse(m_state.parse_result.duration_seconds_fraction, [&]() { return parse_temporal_decimal_fraction(); });

        if (!parse_seconds_designator())
            return false;

        transaction.commit();
        return true;
    }

    // https://tc39.es/ecma262/#prod-TemporalDecimalFraction
    [[nodiscard]] bool parse_temporal_decimal_fraction()
    {
        // TemporalDecimalFraction :::
        //     TemporalDecimalSeparator DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        //     TemporalDecimalSeparator DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit DecimalDigit
        if (!parse_temporal_decimal_separator())
            return false;
        if (!parse_decimal_digit())
            return false;

        for (size_t i = 0; i < 8; ++i) {
            if (!parse_decimal_digit())
                break;
        }

        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-Alpha
    [[nodiscard]] bool parse_alpha()
    {
        // Alpha ::: one of
        //     A B C D E F G H I J K L M N O P Q R S T U V W X Y Z a b c d e f g h i j k l m n o p q r s t u v w x y z
        if (m_state.lexer.next_is(is_ascii_alpha)) {
            m_state.lexer.consume();
            return true;
        }
        return false;
    }

    // https://tc39.es/proposal-temporal/#prod-LowercaseAlpha
    [[nodiscard]] bool parse_lowercase_alpha()
    {
        // LowercaseAlpha ::: one of
        //     a b c d e f g h i j k l m n o p q r s t u v w x y z
        if (m_state.lexer.next_is(is_ascii_lower_alpha)) {
            m_state.lexer.consume();
            return true;
        }
        return false;
    }

    // https://tc39.es/ecma262/#prod-DecimalDigit
    [[nodiscard]] bool parse_decimal_digit()
    {
        // DecimalDigit : one of
        //     0 1 2 3 4 5 6 7 8 9
        if (m_state.lexer.next_is(is_ascii_digit)) {
            m_state.lexer.consume();
            return true;
        }
        return false;
    }

    // https://tc39.es/ecma262/#prod-DecimalDigits
    [[nodiscard]] bool parse_decimal_digits(Separator separator, Optional<StringView>& result)
    {
        StateTransaction transaction { *this };

        // FIXME: Implement [+Sep] if it's ever needed.
        VERIFY(separator == Separator::No);

        // DecimalDigits[Sep] ::
        //     DecimalDigit
        //     DecimalDigits[?Sep] DecimalDigit
        //     [+Sep] DecimalDigits[+Sep] NumericLiteralSeparator DecimalDigit
        if (!parse_decimal_digit())
            return {};
        while (parse_decimal_digit())
            ;

        result = transaction.parsed_string_view();

        transaction.commit();
        return true;
    }

    // https://tc39.es/ecma262/#prod-NonZeroDigit
    [[nodiscard]] bool parse_non_zero_digit()
    {
        // NonZeroDigit : one of
        //     1 2 3 4 5 6 7 8 9
        if (m_state.lexer.next_is(is_ascii_digit) && !m_state.lexer.next_is('0')) {
            m_state.lexer.consume();
            return true;
        }
        return false;
    }

    // https://tc39.es/ecma262/#prod-ASCIISign
    [[nodiscard]] bool parse_ascii_sign()
    {
        // ASCIISign : one of
        //     + -
        return m_state.lexer.consume_specific('+') || m_state.lexer.consume_specific('-');
    }

    // https://tc39.es/proposal-temporal/#prod-DateSeparator
    [[nodiscard]] bool parse_date_separator(Extended extended)
    {
        // DateSeparator[Extended] :::
        //     [+Extended] -
        //     [~Extended] [empty]
        if (extended == Extended::Yes)
            return m_state.lexer.consume_specific('-');
        return true;
    }

    // https://tc39.es/ecma262/#prod-TimeSeparator
    [[nodiscard]] bool parse_time_separator(Extended extended)
    {
        // TimeSeparator[Extended] :::
        //     [+Extended] :
        //     [~Extended] [empty]
        if (extended == Extended::Yes)
            return m_state.lexer.consume_specific(':');
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-TimeDesignator
    [[nodiscard]] bool parse_time_designator()
    {
        // TimeDesignator : one of
        //     T t
        return m_state.lexer.consume_specific('T') || m_state.lexer.consume_specific('t');
    }

    // https://tc39.es/proposal-temporal/#prod-DateTimeSeparator
    [[nodiscard]] bool parse_date_time_separator()
    {
        // DateTimeSeparator :::
        //     <SP>
        //     T
        //     t
        return m_state.lexer.consume_specific(' ') || m_state.lexer.consume_specific('T') || m_state.lexer.consume_specific('t');
    }

    // https://tc39.es/ecma262/#prod-TemporalDecimalSeparator
    [[nodiscard]] bool parse_temporal_decimal_separator()
    {
        // TemporalDecimalSeparator ::: one of
        //    . ,
        return m_state.lexer.consume_specific('.') || m_state.lexer.consume_specific(',');
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDesignator
    [[nodiscard]] bool parse_duration_designator()
    {
        // DurationDesignator : one of
        //     P p
        return m_state.lexer.consume_specific('P') || m_state.lexer.consume_specific('p');
    }

    // https://tc39.es/proposal-temporal/#prod-YearsDesignator
    [[nodiscard]] bool parse_years_designator()
    {
        // YearsDesignator : one of
        //     Y y
        return m_state.lexer.consume_specific('Y') || m_state.lexer.consume_specific('y');
    }

    // https://tc39.es/proposal-temporal/#prod-MonthsDesignator
    [[nodiscard]] bool parse_months_designator()
    {
        // MonthsDesignator : one of
        //     M m
        return m_state.lexer.consume_specific('M') || m_state.lexer.consume_specific('m');
    }

    // https://tc39.es/proposal-temporal/#prod-WeeksDesignator
    [[nodiscard]] bool parse_weeks_designator()
    {
        // WeeksDesignator : one of
        //     W w
        return m_state.lexer.consume_specific('W') || m_state.lexer.consume_specific('w');
    }

    // https://tc39.es/proposal-temporal/#prod-DaysDesignator
    [[nodiscard]] bool parse_days_designator()
    {
        // DaysDesignator : one of
        //     D d
        return m_state.lexer.consume_specific('D') || m_state.lexer.consume_specific('d');
    }

    // https://tc39.es/proposal-temporal/#prod-HoursDesignator
    [[nodiscard]] bool parse_hours_designator()
    {
        // HoursDesignator : one of
        //     H h
        return m_state.lexer.consume_specific('H') || m_state.lexer.consume_specific('h');
    }

    // https://tc39.es/proposal-temporal/#prod-MinutesDesignator
    [[nodiscard]] bool parse_minutes_designator()
    {
        // MinutesDesignator : one of
        //     M m
        return m_state.lexer.consume_specific('M') || m_state.lexer.consume_specific('m');
    }

    // https://tc39.es/proposal-temporal/#prod-SecondsDesignator
    [[nodiscard]] bool parse_seconds_designator()
    {
        // SecondsDesignator : one of
        //     S s
        return m_state.lexer.consume_specific('S') || m_state.lexer.consume_specific('s');
    }

    // https://tc39.es/proposal-temporal/#prod-UTCDesignator
    [[nodiscard]] bool parse_utc_designator()
    {
        StateTransaction transaction { *this };

        // UTCDesignator : one of
        //     Z z
        auto success = m_state.lexer.consume_specific('Z') || m_state.lexer.consume_specific('z');
        if (!success)
            return false;

        m_state.parse_result.utc_designator = transaction.parsed_string_view();
        transaction.commit();
        return true;
    }

    // https://tc39.es/proposal-temporal/#prod-AnnotationCriticalFlag
    [[nodiscard]] bool parse_annotation_critical_flag()
    {
        // AnnotationCriticalFlag :::
        //     !
        return m_state.lexer.consume_specific('!');
    }

private:
    template<typename Parser, typename T>
    [[nodiscard]] bool scoped_parse(Optional<T>& storage, Parser&& parser)
    {
        StateTransaction transaction { *this };

        if (!parser())
            return false;

        if constexpr (IsSame<T, char>)
            storage = transaction.parsed_string_view()[0];
        else
            storage = transaction.parsed_string_view();

        transaction.commit();
        return true;
    }

    struct State {
        GenericLexer lexer;
        ParseResult parse_result;
    };

    struct StateTransaction {
        explicit StateTransaction(ISO8601Parser& parser)
            : m_parser(parser)
            , m_saved_state(parser.m_state)
            , m_start_index(parser.m_state.lexer.tell())
        {
        }

        ~StateTransaction()
        {
            if (!m_commit)
                m_parser.m_state = move(m_saved_state);
        }

        void commit() { m_commit = true; }
        StringView parsed_string_view() const
        {
            return m_parser.m_input.substring_view(m_start_index, m_parser.m_state.lexer.tell() - m_start_index);
        }

    private:
        ISO8601Parser& m_parser;
        State m_saved_state;
        size_t m_start_index { 0 };
        bool m_commit { false };
    };

    StringView m_input;
    State m_state;
};

#define JS_ENUMERATE_ISO8601_PRODUCTION_PARSERS                                        \
    __JS_ENUMERATE(AnnotationValue, parse_annotation_value)                            \
    __JS_ENUMERATE(DateMonth, parse_date_month)                                        \
    __JS_ENUMERATE(TemporalDateTimeString, parse_temporal_date_time_string)            \
    __JS_ENUMERATE(TemporalDurationString, parse_temporal_duration_string)             \
    __JS_ENUMERATE(TemporalInstantString, parse_temporal_instant_string)               \
    __JS_ENUMERATE(TemporalMonthDayString, parse_temporal_month_day_string)            \
    __JS_ENUMERATE(TemporalTimeString, parse_temporal_time_string)                     \
    __JS_ENUMERATE(TemporalYearMonthString, parse_temporal_year_month_string)          \
    __JS_ENUMERATE(TemporalZonedDateTimeString, parse_temporal_zoned_date_time_string) \
    __JS_ENUMERATE(TimeZoneIdentifier, parse_time_zone_identifier)

Optional<ParseResult> parse_iso8601(Production production, StringView input)
{
    ISO8601Parser parser { input };

    switch (production) {
#define __JS_ENUMERATE(ProductionName, parse_production) \
    case Production::ProductionName:                     \
        if (!parser.parse_production())                  \
            return {};                                   \
        break;
        JS_ENUMERATE_ISO8601_PRODUCTION_PARSERS
#undef __JS_ENUMERATE
    default:
        VERIFY_NOT_REACHED();
    }

    // If we parsed successfully but didn't reach the end, the string doesn't match the given production.
    if (!parser.lexer().is_eof())
        return {};

    return parser.parse_result();
}

Optional<TimeZoneOffset> parse_utc_offset(StringView input, SubMinutePrecision sub_minute_precision)
{
    ISO8601Parser parser { input };

    Optional<TimeZoneOffset> utc_offset;

    if (!parser.parse_utc_offset(sub_minute_precision, utc_offset))
        return {};

    // If we parsed successfully but didn't reach the end, the string doesn't match the given production.
    if (!parser.lexer().is_eof())
        return {};

    return utc_offset;
}

}
