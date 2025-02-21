/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/DateRegex.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>

// DateRegex contains a number of patterns created from String constants.
// This test confirms they are exactly like the ones built recursively,
// from parts, by DateRegexGenerator.
TEST_CASE(confirm_generated_patterns)
{
    auto const date_patterns = DateRegexGenerator::create_date_patterns();

    EXPECT_EQ(DateRegex::ecma_script_datetime(), date_patterns.ecma_script_datetime);
    EXPECT_EQ(DateRegex::iso8601_simplified_datetime(), date_patterns.iso8601_simplified_datetime);
    EXPECT_EQ(DateRegex::date_tostring(), date_patterns.date_tostring);
    EXPECT_EQ(DateRegex::date_toutcstring(), date_patterns.date_toutcstring);
    EXPECT_EQ(DateRegex::es_date_parse(), date_patterns.date_parse);
}

// DateConstructor uses only one of the DateRegex patterns: DateRegex::es_date_parse()
// Testing DateRegex::es_date_parse will be in the JS wrapper: Libraries/LibJS/Tests/builtins/Date/Date.parse.nonStandard.js
// The next test cases will test the other ones
TEST_CASE(iso8601_simplified_datetime)
{
    DateRegex const e1 { DateRegex::ecma_script_datetime() };
    DateRegex const e2 { DateRegex::iso8601_simplified_datetime() };
    DateRegex const e3 { DateRegex::es_date_parse() }; // Permissive; should capture all above.

    for (auto const& e : { e1, e2, e3 }) {
        EXPECT_EQ(e.parse("1970-01-01T00:00:00.000Z"_string), 0);
        EXPECT_EQ(e.parse("2021-02-03T12:34:56.789Z"_string), 1612355696789);
        EXPECT_EQ(e.parse("2009-02-13T23:31:30.000Z"_string), 1234567890000);
        EXPECT_EQ(e.parse("2009-02-13T23:31:30.987Z"_string), 1234567890987);

        // https://tc39.es/ecma262/#sec-date.parse:
        // "When the UTC offset representation is absent, date-only forms are interpreted as a UTC time ..."
        // size_t const n_dates=3;
        std::pair<String, String> const dates[] = {
            { "2021-02-03T00:00:00.000Z"_string, "2021-02-03"_string },
            { "2021-02-01T00:00:00.000Z"_string, "2021-02"_string },
            { "2021-01-01T00:00:00.000Z"_string, "2021"_string },
        };

        for (auto const& d : dates) {
            EXPECT_EQ(e.parse(d.first), e.parse(d.second));
        }

        // "...and date-time forms are interpreted as a local time."
        std::pair<String, String> const dates_utc[] = {
            { "2021-02-03T12:34:56.789Z"_string, "2021-02-03T12:34:56.789"_string },
            { "2021-02-01T12:34:56.789Z"_string, "2021-02T12:34:56.789"_string },
            { "2021-01-01T12:34:56.789Z"_string, "2021T12:34:56.789"_string },
            { "2021-02-03T12:34:56.000Z"_string, "2021-02-03T12:34:56"_string },
            { "2021-02-01T12:34:56.000Z"_string, "2021-02T12:34:56"_string },
            { "2021-01-01T12:34:56.000Z"_string, "2021T12:34:56"_string },
            { "2021-02-03T12:34:00.000Z"_string, "2021-02-03T12:34"_string },
            { "2021-02-01T12:34:00.000Z"_string, "2021-02T12:34"_string },
            { "2021-01-01T12:34:00.000Z"_string, "2021T12:34"_string },
        };

        for (auto const& d : dates_utc)
            EXPECT_EQ(JS::utc_time(e.parse(d.first)), e.parse(d.second));

        std::pair<String, String> const dates_tz_utc[] {
            { "2021-02-03T12:34:56.789Z"_string, "2021-02-03T12:34:56.789Z"_string }, // trivial
            { "2021-02-01T12:34:56.789Z"_string, "2021-02T12:34:56.789Z"_string },
            { "2021-01-01T12:34:56.789Z"_string, "2021T12:34:56.789Z"_string },
            { "2021-02-03T12:34:56.000Z"_string, "2021-02-03T12:34:56Z"_string },
            { "2021-02-01T12:34:56.000Z"_string, "2021-02T12:34:56Z"_string },
            { "2021-01-01T12:34:56.000Z"_string, "2021T12:34:56Z"_string },
            { "2021-02-03T12:34:00.000Z"_string, "2021-02-03T12:34Z"_string },
            { "2021-02-01T12:34:00.000Z"_string, "2021-02T12:34Z"_string },
            { "2021-01-01T12:34:00.000Z"_string, "2021T12:34Z"_string },
            //
            // h24 == h00 + 1d
            { "2021-02-03T00:00:00.000Z"_string, "2021-02-02T24:00:00.000Z"_string },
            // 2021 is not a leap year
            { "2021-03-01T12:34:56.789Z"_string, "2021-02-29T12:34:56.789Z"_string },
        };

        for (auto const& d : dates_tz_utc) {
            EXPECT_EQ(e1.parse(d.first), e.parse(d.second));
        }

        std::pair<String, String> const dates_tz[] {
            // Timezone offset +HH:MM
            { "2021-02-03T07:34:56.789Z"_string, "2021-02-03T12:34:56.789+05:00"_string },
            { "2021-02-03T23:34:56.789Z"_string, "2021-02-03T12:34:56.789-11:00"_string },
            { "2021-02-01T07:34:56.789Z"_string, "2021-02T12:34:56.789+05:00"_string },
            { "2021-02-01T23:34:56.789Z"_string, "2021-02T12:34:56.789-11:00"_string },
            { "2021-01-01T07:34:56.789Z"_string, "2021T12:34:56.789+05:00"_string },
            { "2021-01-01T23:34:56.789Z"_string, "2021T12:34:56.789-11:00"_string },
            { "2021-02-03T07:34:56.000Z"_string, "2021-02-03T12:34:56+05:00"_string },
            { "2021-02-03T23:34:56.000Z"_string, "2021-02-03T12:34:56-11:00"_string },
            { "2021-02-01T07:34:56.000Z"_string, "2021-02T12:34:56+05:00"_string },
            { "2021-02-01T23:34:56.000Z"_string, "2021-02T12:34:56-11:00"_string },
            { "2021-02-03T07:34:56.789Z"_string, "2021T12:34:56+05:00"_string },
            { "2021-02-03T23:34:56.789Z"_string, "2021T12:34:56-11:00"_string },
            { "2021-02-03T07:34:00.789Z"_string, "2021-02-03T12:34+05:00"_string },
            { "2021-02-03T23:34:00.789Z"_string, "2021-02-03T12:34-11:00"_string },
            { "2021-02-01T07:34:00.789Z"_string, "2021-02T12:34+05:00"_string },
            { "2021-02-01T23:34:00.789Z"_string, "2021-02T12:34-11:00"_string },
            { "2021-01-01T07:34:00.789Z"_string, "2021T12:34+05:00"_string },
            { "2021-01-01T23:34:00.789Z"_string, "2021T12:34-11:00"_string },
            // Timezone offset +HHMM ("military")
            { "2021-02-03T07:34:56.789Z"_string, "2021-02-03T12:34:56.789+0500"_string },
            { "2021-02-03T23:34:56.789Z"_string, "2021-02-03T12:34:56.789-1100"_string },
            { "2021-02-01T07:34:56.789Z"_string, "2021-02T12:34:56.789+0500"_string },
            { "2021-02-01T23:34:56.789Z"_string, "2021-02T12:34:56.789-1100"_string },
            { "2021-01-01T07:34:56.789Z"_string, "2021T12:34:56.789+0500"_string },
            { "2021-01-01T23:34:56.789Z"_string, "2021T12:34:56.789-1100"_string },
            { "2021-02-03T07:34:56.000Z"_string, "2021-02-03T12:34:56+0500"_string },
            { "2021-02-03T23:34:56.000Z"_string, "2021-02-03T12:34:56-1100"_string },
            { "2021-02-01T07:34:56.000Z"_string, "2021-02T12:34:56+0500"_string },
            { "2021-02-01T23:34:56.000Z"_string, "2021-02T12:34:56-1100"_string },
            { "2021-02-03T07:34:56.789Z"_string, "2021T12:34:56+0500"_string },
            { "2021-02-03T23:34:56.789Z"_string, "2021T12:34:56-1100"_string },
            { "2021-02-03T07:34:00.789Z"_string, "2021-02-03T12:34+0500"_string },
            { "2021-02-03T23:34:00.789Z"_string, "2021-02-03T12:34-1100"_string },
            { "2021-02-01T07:34:00.789Z"_string, "2021-02T12:34+0500"_string },
            { "2021-02-01T23:34:00.789Z"_string, "2021-02T12:34-1100"_string },
            { "2021-01-01T07:34:00.789Z"_string, "2021T12:34+0500"_string },
            { "2021-01-01T23:34:00.789Z"_string, "2021T12:34-1100"_string },
        };

        for (auto const& d : dates_tz_utc) {
            EXPECT_EQ(e1.parse(d.first), e.parse(d.second));
        }
    }

    // ecma_script_datetime without ISO8601 extensions
    EXPECT(std::isnan(e1.parse("+002021-02-03T12:34:56.123Z"_string)));
    EXPECT(std::isnan(e1.parse("-002021-02-03T12:34:56.123Z"_string)));
    EXPECT(std::isnan(e1.parse("+000000-02-03T12:34:56.123Z"_string)));
    EXPECT(std::isnan(e1.parse("-000000-02-03T12:34:56.123Z"_string)));
    EXPECT(std::isnan(e1.parse("2021-02-03T12:34:56.123456789Z"_string)));

    // "The representation of the year 0 as -000000 is invalid." https://tc39.es/ecma262/#sec-expanded-years
    EXPECT(std::isnan(e2.parse("-000000-02-03T12:34:56.123Z"_string)));

    // ISO8601 extensions
    for (auto const& e : { e2, e3 }) {
        // More than three digits for fractional seconds (Compatible with Firefox and Chrome)
        // More permissive than the standard, but very convoluted examples indicate that this
        // is likely parsed by Chrome and Firefox as a simplified ISO8601 format
        // https://tc39.es/ecma262/#sec-date.parse
        EXPECT_EQ(e.parse("2021-02-03T12:34:56.123456789Z"_string), 1612355696123); // truncated

        // Expanded year (6-digit signed year) https://tc39.es/ecma262/#sec-expanded-years
        EXPECT_EQ(e.parse("+002021-02-03T12:34:56.123Z"_string), 1612355696123);
        EXPECT_EQ(e.parse("-002021-02-03T12:34:56.123Z"_string), -125940914703877);
        EXPECT_EQ(e.parse("+000000-02-03T12:34:56.123Z"_string), -62164322703877);

        // [Extended] time zone offset string format +HH:MM:SS.Ns https://tc39.es/ecma262/#sec-time-zone-offset-strings
        std::pair<String, String> const dates_tz[] {
            { "2021-02-03T10:00:00.789Z"_string, "2021-02-03T12:34:56.789+02:34:56"_string },
            { "2021-02-03T10:00:00.666Z"_string, "2021-02-03T12:34:56.789+02:34:56.123"_string },
            { "2021-02-03T10:00:00.666Z"_string, "2021-02-03T12:34:56.789+02:34:56.123456789"_string },
        };

        for (auto const& d : dates_tz) {
            EXPECT_EQ(e1.parse(d.first), e.parse(d.second));
        }
    }
}

TEST_CASE(date_tostring)
{
    DateRegex const e1 { DateRegex::ecma_script_datetime() }; // reference

    // Test these dates:
    DateRegex const e2 { DateRegex::date_tostring() };
    DateRegex const e3 { DateRegex::es_date_parse() };

    for (auto const& e : { e2, e3 }) {
        std::pair<String, String> const dates[] {
            { "1999-12-08T12:34:56.000Z"_string, "Wed Dec 08 1999 12:34:56 GMT"_string },
            { "1999-12-08T20:34:56.000Z"_string, "Wed Dec 08 1999 12:34:56 GMT-0800"_string },
            { "1999-12-08T20:34:56.000Z"_string, "Wed Dec 08 1999 12:34:56 GMT-0800 (Pacific Standard Time)"_string },
            { "1999-12-08T12:34:56.000Z"_string, "Sat Dec 08 1999 12:34:56 GMT"_string },                              // wrong weekday (weekday is ignored)
            { "1999-12-08T20:34:56.000Z"_string, "Wed Dec 08 1999 12:34:56 GMT-0800 (Eastern Daylight Time)"_string }, // wrong timezone name (timezone name is ignored)
        };

        for (auto const& d : dates) {
            EXPECT_EQ(e1.parse(d.first), e.parse(d.second));
        }
    }

    // Date.toString() produces correct weekday 3-letter abbreviations, even if they are ignored when parsing.
    // Anything else fails.
    EXPECT(isnan(e2.parse("Wednesday Dec 08 1999 12:34:56 GMT"_string)));
    EXPECT(isnan(e2.parse("Wee Dec 08 1999 12:34:56 GMT"_string)));

    // But the permissive mode accepts anything for weekdays
    EXPECT_EQ(e3.parse("Wednesday Dec 08 1999 12:34:56 GMT"_string), e1.parse("1999-12-08T12:34:56.000Z"_string));
    EXPECT_EQ(e3.parse("Wee Dec 08 1999 12:34:56 GMT"_string), e1.parse("1999-12-08T12:34:56.000Z"_string));
    EXPECT_EQ(e3.parse("Weddding Dec 08 1999 12:34:56 GMT"_string), e1.parse("1999-12-08T12:34:56.000Z"_string));
}

TEST_CASE(date_toutcstring)
{
    DateRegex const e1 { DateRegex::ecma_script_datetime() }; // reference

    // Test these ones:
    DateRegex const e2 { DateRegex::date_toutcstring() };
    DateRegex const e3 { DateRegex::es_date_parse() };

    for (auto const& e : { e2, e3 }) {
        std::pair<String, String> const dates[] {
            { "1986-04-08T12:34:56.000Z"_string, "Tue, 08 Apr 1986 12:34:56 GMT"_string }, // Clint Eastwood elected mayor of Carmel
            { "1986-04-08T12:34:56.000Z"_string, "Sun, 08 Apr 1986 12:34:56 GMT"_string },
        };

        for (auto const& d : dates) {
            EXPECT_EQ(e1.parse(d.first), e.parse(d.second));
        }
    }
    // Date.toUTCString() produces correct weekday 3-letter abbreviations, even if they are ignored when parsing.
    // Anything else fails.
    EXPECT(isnan(e2.parse("Tuesday, 08 Apr 1986 12:34:56 GMT"_string)));
    EXPECT(isnan(e2.parse("Tee Dec 08 1999 12:34:56 GMT"_string)));

    // But the permissive mode accepts anything for weekdays
    EXPECT_EQ(e3.parse("Tuesday, 08 Apr 1986 12:34:56 GMT"_string), e1.parse("1986-04-08T12:34:56.000Z"_string));
    EXPECT_EQ(e3.parse("Tee, 08 Apr 1986 12:34:56 GMT"_string), e1.parse("1986-04-08T12:34:56.000Z"_string));
    EXPECT_EQ(e3.parse("Tunesdays, 08 Apr 1986 12:34:56 GMT"_string), e1.parse("1986-04-08T12:34:56.000Z"_string));
}
