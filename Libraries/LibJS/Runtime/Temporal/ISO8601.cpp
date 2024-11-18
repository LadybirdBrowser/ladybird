/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>

namespace JS::Temporal {

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

    enum class Separator {
        No,
        Yes,
    };

    // https://tc39.es/proposal-temporal/#prod-TemporalDurationString
    [[nodiscard]] bool parse_temporal_duration_string()
    {
        // TemporalDurationString :
        //     Duration
        return parse_duration();
    }

    // https://tc39.es/proposal-temporal/#prod-DurationDate
    [[nodiscard]] bool parse_duration_date()
    {
        // DurationDate :
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
        (void)parse_ascii_sign();

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

        // DurationYearsPart :
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

        // DurationMonthsPart :
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

        // DurationWeeksPart :
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

        // DurationDaysPart :
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

        // DurationTime :
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

        // DurationHoursPart :
        //     DecimalDigits[~Sep] TemporalDecimalFraction HoursDesignator
        //     DecimalDigits[~Sep] HoursDesignator DurationMinutesPart
        //     DecimalDigits[~Sep] HoursDesignator DurationSecondsPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_hours))
            return false;

        auto is_fractional = parse_temporal_decimal_fraction(m_state.parse_result.duration_hours_fraction);

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

        // DurationMinutesPart :
        //     DecimalDigits[~Sep] TemporalDecimalFraction MinutesDesignator
        //     DecimalDigits[~Sep] MinutesDesignator DurationSecondsPart[opt]
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_minutes))
            return false;

        auto is_fractional = parse_temporal_decimal_fraction(m_state.parse_result.duration_minutes_fraction);

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

        // DurationSecondsPart :
        //     DecimalDigits[~Sep] TemporalDecimalFraction[opt] SecondsDesignator
        if (!parse_decimal_digits(Separator::No, m_state.parse_result.duration_seconds))
            return false;

        (void)parse_temporal_decimal_fraction(m_state.parse_result.duration_seconds_fraction);

        if (!parse_seconds_designator())
            return false;

        transaction.commit();
        return true;
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

    // https://tc39.es/ecma262/#prod-TemporalDecimalFraction
    [[nodiscard]] bool parse_temporal_decimal_fraction(Optional<StringView>& result)
    {
        StateTransaction transaction { *this };

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

        result = transaction.parsed_string_view();

        transaction.commit();
        return true;
    }

    // https://tc39.es/ecma262/#prod-ASCIISign
    [[nodiscard]] bool parse_ascii_sign()
    {
        StateTransaction transaction { *this };

        // ASCIISign : one of
        //     + -
        if (!m_state.lexer.next_is(is_any_of("+-"sv)))
            return false;

        m_state.parse_result.sign = m_state.lexer.consume();

        transaction.commit();
        return true;
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

    // https://tc39.es/proposal-temporal/#prod-TimeDesignator
    [[nodiscard]] bool parse_time_designator()
    {
        // TimeDesignator : one of
        //     T t
        return m_state.lexer.consume_specific('T') || m_state.lexer.consume_specific('t');
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

private:
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

#define JS_ENUMERATE_ISO8601_PRODUCTION_PARSERS \
    __JS_ENUMERATE(TemporalDurationString, parse_temporal_duration_string)

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

}
