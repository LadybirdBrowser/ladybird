/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NOTE: Using ISOString rather than "milliseconds since epoch" in test cases, for readability.
// The result ISOString on the left ("expect") side, so that the source strings are nicely aligned.

// NOTE: Using "new Date" vs. Date.parse for brevity in most tests, since:
// "[new Date returns] the result of parsing v as a date, in exactly the same manner as for the parse method"
// https://tc39.es/ecma262/#sec-date

// NOTE: I decided to do setTimeZone("America/Vancouver") vs. setTimeZone("PST"), since the former
// flips between PST/PDT (ok) and the latter overrides PDT seasonally (arguably incorrect).

// NOTE: After 1884, "America/Vancouver" is PST. After 1941 PDT is added seasonally.
// Before 1884 it is Local Mean Time (Vancouver); 12min 28s offset vs. PST
// Don't be alarmed if you see it in some of the examples. Firefox and Chrome behave the same way.
// https://en.wikipedia.org/wiki/Standard_time#North_America

test("canonical format: ECMA date time string format", () => {
    // https://tc39.es/ecma262/#sec-date-time-string-format
    const originalTimeZone = setTimeZone("America/Vancouver");

    // No time zone
    // Date only is GMT
    expect("2023-01-01T00:00:00.000Z").toBe(new Date("2023").toISOString());
    expect("2023-10-01T00:00:00.000Z").toBe(new Date("2023-10").toISOString());
    expect("2023-10-11T00:00:00.000Z").toBe(new Date("2023-10-11").toISOString());

    // Strings that are not compliant with the ECMA date time string format
    // (e.g. slash or space date separators) are interpreted as local time
    expect("2023-10-11T07:00:00.000Z").toBe(new Date("2023/10/11").toISOString());
    expect("2023-10-11T07:00:00.000Z").toBe(new Date("2023 10 11").toISOString());

    // Date-time
    expect("2023-01-01T20:34:00.000Z").toBe(new Date("2023T12:34").toISOString()); // Daylight
    expect("2023-10-01T19:34:00.000Z").toBe(new Date("2023-10T12:34").toISOString());
    expect("2023-10-11T19:34:00.000Z").toBe(new Date("2023-10-11T12:34").toISOString());

    expect("2023-01-01T20:34:56.000Z").toBe(new Date("2023T12:34:56").toISOString());
    expect("2023-10-01T19:34:56.000Z").toBe(new Date("2023-10T12:34:56").toISOString());
    expect("2023-10-11T19:34:56.000Z").toBe(new Date("2023-10-11T12:34:56").toISOString());

    expect("2023-01-01T20:34:56.789Z").toBe(new Date("2023T12:34:56.789").toISOString());
    expect("2023-10-01T19:34:56.789Z").toBe(new Date("2023-10T12:34:56.789").toISOString());
    expect("2023-10-11T19:34:56.789Z").toBe(new Date("2023-10-11T12:34:56.789").toISOString());

    // Z
    expect("2023-01-01T12:34:00.000Z").toBe(new Date("2023T12:34Z").toISOString());
    expect("2023-10-01T12:34:00.000Z").toBe(new Date("2023-10T12:34Z").toISOString());
    expect("2023-10-11T12:34:00.000Z").toBe(new Date("2023-10-11T12:34Z").toISOString());

    expect("2023-01-01T12:34:56.000Z").toBe(new Date("2023T12:34:56Z").toISOString());
    expect("2023-10-01T12:34:56.000Z").toBe(new Date("2023-10T12:34:56Z").toISOString());
    expect("2023-10-11T12:34:56.000Z").toBe(new Date("2023-10-11T12:34:56Z").toISOString());

    expect("2023-01-01T12:34:56.789Z").toBe(new Date("2023T12:34:56.789Z").toISOString());
    expect("2023-10-01T12:34:56.789Z").toBe(new Date("2023-10T12:34:56.789Z").toISOString());
    expect("2023-10-11T12:34:56.789Z").toBe(new Date("2023-10-11T12:34:56.789Z").toISOString());

    // Timezone offset HHMM[SS] ("military")
    expect("1980-01-01T10:34:00.000Z").toBe(new Date("1980T12:34+0200").toISOString());
    expect("1980-10-01T10:34:00.000Z").toBe(new Date("1980-10T12:34+0200").toISOString());
    expect("1980-10-11T10:34:00.000Z").toBe(new Date("1980-10-11T12:34+0200").toISOString());

    expect("1980-01-01T10:34:56.000Z").toBe(new Date("1980T12:34:56+0200").toISOString());
    expect("1980-10-01T10:34:56.000Z").toBe(new Date("1980-10T12:34:56+0200").toISOString());
    expect("1980-10-11T10:34:56.000Z").toBe(new Date("1980-10-11T12:34:56+0200").toISOString());

    expect("1980-01-01T10:34:56.789Z").toBe(new Date("1980T12:34:56.789+0200").toISOString());
    expect("1980-10-01T10:34:56.789Z").toBe(new Date("1980-10T12:34:56.789+0200").toISOString());
    expect("1980-10-11T10:34:56.789Z").toBe(new Date("1980-10-11T12:34:56.789+0200").toISOString());

    expect("1980-10-11T10:34:54.789Z").toBe(
        new Date("1980-10-11T12:34:56.789+020002").toISOString()
    );

    // Timezone offset HH:MM
    expect("1980-01-01T10:34:00.000Z").toBe(new Date("1980T12:34+02:00").toISOString());
    expect("1980-10-01T10:34:00.000Z").toBe(new Date("1980-10T12:34+02:00").toISOString());
    expect("1980-10-11T10:34:00.000Z").toBe(new Date("1980-10-11T12:34+02:00").toISOString());

    expect("1980-01-01T10:34:56.000Z").toBe(new Date("1980T12:34:56+02:00").toISOString());
    expect("1980-10-01T10:34:56.000Z").toBe(new Date("1980-10T12:34:56+02:00").toISOString());
    expect("1980-10-11T10:34:56.000Z").toBe(new Date("1980-10-11T12:34:56+02:00").toISOString());

    expect("1980-01-01T10:34:56.789Z").toBe(new Date("1980T12:34:56.789+02:00").toISOString());
    expect("1980-10-01T10:34:56.789Z").toBe(new Date("1980-10T12:34:56.789+02:00").toISOString());
    expect("1980-10-11T10:34:56.789Z").toBe(
        new Date("1980-10-11T12:34:56.789+02:00").toISOString()
    );

    setTimeZone(originalTimeZone);
});

test('canonical format: ECMA + ISO8601 extensions ("simplified" ISO8601)', () => {
    const originalTimeZone = setTimeZone("America/Vancouver");
    // Timezone offset HH:MM:SS
    // https://tc39.es/ecma262/#sec-time-zone-offset-strings
    // NOTE: At this time, neither Firefox nor Chrome support nanoseconds in the timezone offset. We do.
    expect("1980-01-01T10:33:15.000Z").toBe(new Date("1980T12:34+02:00:45").toISOString());
    expect("1980-10-01T10:33:15.000Z").toBe(new Date("1980-10T12:34+02:00:45").toISOString());
    expect("1980-10-11T10:33:15.000Z").toBe(new Date("1980-10-11T12:34+02:00:45").toISOString());

    expect("1980-01-01T10:34:11.000Z").toBe(new Date("1980T12:34:56+02:00:45").toISOString());
    expect("1980-10-01T10:34:11.000Z").toBe(new Date("1980-10T12:34:56+02:00:45").toISOString());
    expect("1980-10-11T10:34:11.000Z").toBe(new Date("1980-10-11T12:34:56+02:00:45").toISOString());

    expect("1980-01-01T10:34:11.789Z").toBe(new Date("1980T12:34:56.789+02:00:45").toISOString());
    expect("1980-10-01T10:34:11.789Z").toBe(
        new Date("1980-10T12:34:56.789+02:00:45").toISOString()
    );
    expect("1980-10-11T10:34:11.789Z").toBe(
        new Date("1980-10-11T12:34:56.789+02:00:45").toISOString()
    );

    // The ECMA date-time string format requires literal uppercase 'T' and 'Z'.
    // Chrome also accepts lowercase.
    // Firefox and us only accept uppercase.
    expect(Date.parse("2001-02-03t12:34:56.123z")).toBeNaN();
    expect(Date.parse("2001-02-03t12:34:56.123Z")).toBeNaN();
    expect(Date.parse("2001-02-03T12:34:56.123z")).toBeNaN();

    // Timezone offset HH:MM:SS.Ns
    expect("1980-01-01T10:33:14.322Z").toBe(new Date("1980T12:34+02:00:45.678").toISOString());
    expect("1980-10-01T10:33:14.322Z").toBe(new Date("1980-10T12:34+02:00:45.678").toISOString());
    expect("1980-10-11T10:33:14.322Z").toBe(
        new Date("1980-10-11T12:34+02:00:45.678").toISOString()
    );

    expect("1980-01-01T10:34:10.322Z").toBe(new Date("1980T12:34:56+02:00:45.678").toISOString());
    expect("1980-10-01T10:34:10.322Z").toBe(
        new Date("1980-10T12:34:56+02:00:45.678").toISOString()
    );
    expect("1980-10-11T10:34:10.322Z").toBe(
        new Date("1980-10-11T12:34:56+02:00:45.678").toISOString()
    );

    expect("1980-01-01T10:34:11.111Z").toBe(
        new Date("1980T12:34:56.789+02:00:45.678").toISOString()
    );
    expect("1980-10-01T10:34:11.111Z").toBe(
        new Date("1980-10T12:34:56.789+02:00:45.678").toISOString()
    );
    expect("1980-10-11T10:34:11.111Z").toBe(
        new Date("1980-10-11T12:34:56.789+02:00:45.678").toISOString()
    );

    expect("1980-01-01T10:34:11.666Z").toBe(
        new Date("1980T12:34:56.789+02:00:45.123456879").toISOString()
    );
    expect("1980-10-01T10:34:11.666Z").toBe(
        new Date("1980-10T12:34:56.789+02:00:45.123456879").toISOString()
    );
    expect("1980-10-11T10:34:11.666Z").toBe(
        new Date("1980-10-11T12:34:56.789+02:00:45.123456879").toISOString()
    );

    // Expanded years https://tc39.es/ecma262/#sec-expanded-years
    expect("2023-01-01T00:00:00.000Z").toBe(new Date("+002023").toISOString());
    expect("2023-10-01T00:00:00.000Z").toBe(new Date("+002023-10").toISOString());
    expect("2023-10-11T00:00:00.000Z").toBe(new Date("+002023-10-11").toISOString());

    expect("2023-10-11T19:34:00.000Z").toBe(new Date("+002023-10-11T12:34").toISOString());
    expect("2023-10-11T19:34:56.000Z").toBe(new Date("+002023-10-11T12:34:56").toISOString());
    expect("2023-10-11T19:34:56.789Z").toBe(new Date("+002023-10-11T12:34:56.789").toISOString());
    expect("2023-10-11T12:34:56.000Z").toBe(new Date("+002023-10-11T12:34:56Z").toISOString());
    expect("2023-10-11T12:34:56.789Z").toBe(new Date("+002023-10-11T12:34:56.789Z").toISOString());
    expect("2023-10-11T10:34:56.789Z").toBe(
        new Date("+002023-10-11T12:34:56.789+0200").toISOString()
    );
    expect("2023-10-11T10:34:56.789Z").toBe(
        new Date("+002023-10-11T12:34:56.789+02:00").toISOString()
    );
    expect("2023-10-11T10:34:11.666Z").toBe(
        new Date("+002023-10-11T12:34:56.789+02:00:45.123").toISOString()
    );
    expect("2023-10-11T10:34:11.666Z").toBe(
        new Date("+002023-10-11T12:34:56.789+02:00:45.123456789").toISOString()
    );

    expect("-002023-10-11T00:00:00.000Z").toBe(new Date("-002023-10-11").toISOString());
    expect("-002023-10-11T20:46:28.000Z").toBe(new Date("-002023-10-11T12:34").toISOString());
    expect("-002023-10-11T20:47:24.000Z").toBe(new Date("-002023-10-11T12:34:56").toISOString());
    expect("-002023-10-11T20:47:24.789Z").toBe(
        new Date("-002023-10-11T12:34:56.789").toISOString()
    );
    expect("-002023-10-11T10:34:56.789Z").toBe(
        new Date("-002023-10-11T12:34:56.789+0200").toISOString()
    );
    expect("-002023-10-11T10:34:56.789Z").toBe(
        new Date("-002023-10-11T12:34:56.789+02:00").toISOString()
    );
    expect("-002023-10-11T10:34:11.789Z").toBe(
        new Date("-002023-10-11T12:34:56.789+02:00:45").toISOString()
    );
    expect("-002023-10-11T10:34:11.666Z").toBe(
        new Date("-002023-10-11T12:34:56.789+02:00:45.123").toISOString()
    );
    expect("-002023-10-11T10:34:11.666Z").toBe(
        new Date("-002023-10-11T12:34:56.789+02:00:45.123456789").toISOString()
    );

    expect("0000-02-03T12:34:56.789Z").toBe(new Date("+000000-02-03T12:34:56.789Z").toISOString());

    // "The representation of the year 0 as -000000 is invalid." https://tc39.es/ecma262/#sec-expanded-years
    // Firefox and Chrome do not agree on parsing it. We fail everywhere.
    expect(Date.parse("-000000-02-03T12:34:56.123Z")).toBeNaN(); // Both Firefox and Chrome fail.
    expect(Date.parse("-000000-02-03")).toBeNaN(); // Firefox: "2000-02-03T08:00:00.000Z"; Chrome: "2001-02-03T08:00:00.000Z". We fail.
    expect(Date.parse("-000000-02")).toBeNaN(); // Firefox fails; Chrome: "2001-02-01T08:00:00.000Z". We fail.
    expect(Date.parse("-000000")).toBeNaN(); // Firefox: "2000-01-01T08:00:00.000Z"; Chrome fails. We fail.

    setTimeZone(originalTimeZone);
});

test("canonical format: Date.toString", () => {
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("1999-12-08T12:34:56.000Z").toBe(new Date("Wed Dec 08 1999 12:34:56 GMT").toISOString()); // Clint Eastwood elected mayor of Carmel
    expect("1999-12-08T20:34:56.000Z").toBe(
        new Date("Wed Dec 08 1999 12:34:56 GMT-0800").toISOString()
    );
    expect("1999-12-08T20:34:56.000Z").toBe(
        new Date("Wed Dec 08 1999 12:34:56 GMT-0800 (Pacific Standard Time)").toISOString()
    );
    expect("1999-12-08T20:34:56.000Z").toBe(
        new Date("Wed Dec 08 1999 12:34:56 GMT-0800 (Central European Time)").toISOString()
    ); // The time zone name is ignored
    expect("1999-12-08T12:34:56.000Z").toBe(new Date("Sat Dec 08 1999 12:34:56 GMT").toISOString()); // Wrong weekday is ignored

    setTimeZone(originalTimeZone);
});

test("canonical format: Date.toUTCString", () => {
    expect("1999-12-08T08:00:00.000Z").toBe(
        new Date("Wed, 08 Dec 1999 08:00:00 GMT").toISOString()
    );
    expect("1999-12-08T08:00:00.000Z").toBe(
        new Date("Thu, 08 Dec 1999 08:00:00 GMT").toISOString()
    ); // Wrong weekday is ignored
});

test("ambiguous date: 1 number", () => {
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("1970-01-01T00:00:00.000Z").toBe(new Date("1970").toISOString()); // GMT because it matches simplified ISO8601

    // Everything else local time
    expect("1970-01-01T08:00:00.000Z").toBe(new Date("-1970").toISOString());
    expect("1970-01-01T08:00:00.000Z").toBe(new Date("/1970").toISOString());
    expect("1970-01-01T08:00:00.000Z").toBe(new Date(".1970").toISOString());
    expect("1970-01-01T08:00:00.000Z").toBe(new Date("+1970").toISOString());
    expect("1970-01-01T08:00:00.000Z").toBe(new Date("./.-/////  /// // 1970").toISOString());
    expect("1970-01-01T08:00:00.000Z").toBe(
        // "Firefox" punctuation is ignored. Chrome is a lot more permissive
        new Date("// ,./.-/////  /// // 1970 ---").toISOString()
    );
    expect(Date.parse("!1970")).toBeNaN(); // Other punctuation is rejected everywhere in the string.
    expect(Date.parse("?1970")).toBeNaN();
    expect(Date.parse("~1970")).toBeNaN();
    expect(Date.parse("{1970")).toBeNaN();
    expect(Date.parse("}1970")).toBeNaN();
    expect(Date.parse("!1970")).toBeNaN();
    expect(Date.parse("@1970")).toBeNaN();
    expect(Date.parse("#1970")).toBeNaN();
    expect(Date.parse("$1970")).toBeNaN();
    expect(Date.parse("&1970")).toBeNaN();
    expect(Date.parse("*1970")).toBeNaN();
    expect(Date.parse("(1970")).toBeNaN();
    expect(Date.parse(")1970")).toBeNaN();
    expect(Date.parse("=1970")).toBeNaN();
    expect(Date.parse("`1970")).toBeNaN();
    expect(Date.parse(">1970")).toBeNaN();
    expect(Date.parse(";1970")).toBeNaN();
    expect(Date.parse("<1970")).toBeNaN();

    expect("2000-01-01T08:00:00.000Z").toBe(new Date("0").toISOString());
    expect("2000-01-01T08:00:00.000Z").toBe(new Date("00").toISOString());
    expect("2000-01-01T08:00:00.000Z").toBe(new Date("000").toISOString());
    expect("2000-01-01T08:00:00.000Z").toBe(new Date("000000").toISOString());

    expect("2001-01-01T08:00:00.000Z").toBe(new Date("01").toISOString()); // 1-12 month number, 2001
    expect("2001-01-01T08:00:00.000Z").toBe(new Date("1").toISOString());
    expect("2001-02-01T08:00:00.000Z").toBe(new Date("2").toISOString());
    expect("2001-12-01T08:00:00.000Z").toBe(new Date("12").toISOString());

    expect(Date.parse("13")).toBeNaN(); // Firefox and Chrome fail on 13-31. We also fail.
    expect(Date.parse("31")).toBeNaN();
    expect(Date.parse("-14")).toBeNaN(); // Punctuation/sign does not make a difference.

    expect("2032-01-01T08:00:00.000Z").toBe(new Date("32").toISOString()); // 32-49 2000+
    expect("2041-01-01T08:00:00.000Z").toBe(new Date("41").toISOString());
    expect("2049-01-01T08:00:00.000Z").toBe(new Date("49").toISOString());
    expect("1950-01-01T08:00:00.000Z").toBe(new Date("50").toISOString()); // 50-99 1900+
    expect("1978-01-01T08:00:00.000Z").toBe(new Date("78").toISOString());
    expect("1999-01-01T08:00:00.000Z").toBe(new Date("99").toISOString());

    // Punctuation is generally ignored.
    expect("2001-01-01T08:00:00.000Z").toBe(new Date("-/--1").toISOString()); // 2001 month number
    expect("2001-11-01T08:00:00.000Z").toBe(new Date("-11").toISOString());
    expect("2032-01-01T08:00:00.000Z").toBe(new Date("-32").toISOString()); // 32-99 year number
    expect("2040-01-01T08:00:00.000Z").toBe(new Date("-40").toISOString());
    expect("1950-01-01T08:00:00.000Z").toBe(new Date("-50").toISOString());
    expect("0100-01-01T08:12:28.000Z").toBe(new Date("-100").toISOString());
    expect("1999-01-01T08:00:00.000Z").toBe(new Date("-99").toISOString());
    expect("1970-01-01T08:00:00.000Z").toBe(new Date("-70").toISOString());

    setTimeZone(originalTimeZone);
});

test("ambiguous date: 2 numbers", () => {
    // Firefox fails on all. Chrome is weird. We fail on all.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect(Date.parse("10 1970")).toBeNaN(); // Chrome also fails.
    expect(Date.parse("1970 12")).toBeNaN();
    expect(Date.parse("1970 7")).toBeNaN();
    expect(Date.parse("2024 7")).toBeNaN();
    expect(Date.parse("2024 12")).toBeNaN();
    expect(Date.parse("1 2")).toBeNaN();
    expect(Date.parse("1 12")).toBeNaN(); // Chrome parses as Jan-12-2001
    expect(Date.parse("1 32")).toBeNaN(); // Chrome also fails
    expect(Date.parse("2 12")).toBeNaN(); // Chrome parses as Feb-12-2001
    expect(Date.parse("32 1")).toBeNaN();
    expect(Date.parse("32 2")).toBeNaN();
    expect(Date.parse("2034 1")).toBeNaN();
    expect(Date.parse("-002002 2")).toBeNaN();
    setTimeZone(originalTimeZone);
});

test("ambiguous date: 3 numbers", () => {
    // Compatible with Firefox and Chrome.
    const originalTimeZone = setTimeZone("America/Vancouver");

    // 3 digits-only default to MDY
    expect("2024-07-07T07:00:00.000Z").toBe(new Date("7 7 2024").toISOString());
    expect("2007-05-06T07:00:00.000Z").toBe(new Date("5 6 7").toISOString());
    expect("2006-11-05T08:00:00.000Z").toBe(new Date("11 5 6").toISOString());
    expect("2006-05-11T07:00:00.000Z").toBe(new Date("5 11 6").toISOString());
    expect("2011-05-06T07:00:00.000Z").toBe(new Date("5 6 11").toISOString());
    expect("2005-10-11T07:00:00.000Z").toBe(new Date("10 11 5").toISOString());
    expect("2011-10-05T07:00:00.000Z").toBe(new Date("10 5 11").toISOString());
    expect("2011-05-10T07:00:00.000Z").toBe(new Date("5 10 11").toISOString());

    expect("2001-05-06T07:00:00.000Z").toBe(new Date("5 6 1").toISOString());
    expect("2012-05-06T07:00:00.000Z").toBe(new Date("5 6 12").toISOString());
    expect("2013-05-06T07:00:00.000Z").toBe(new Date("5 6 13").toISOString());
    expect("2031-05-06T07:00:00.000Z").toBe(new Date("5 6 31").toISOString());
    expect("2032-05-06T07:00:00.000Z").toBe(new Date("5 6 32").toISOString());
    expect("2049-05-06T07:00:00.000Z").toBe(new Date("5 6 49").toISOString()); // Below 50: 2000+
    expect("1950-05-06T07:00:00.000Z").toBe(new Date("5 6 50").toISOString()); // 50 or more: 1900+
    expect("1988-05-06T07:00:00.000Z").toBe(new Date("5 6 88").toISOString());
    expect("1999-05-06T07:00:00.000Z").toBe(new Date("5 6 99").toISOString());

    // YMD if first number can only be a year (with exceptions)
    expect("2024-12-03T08:00:00.000Z").toBe(new Date("2024 12 3").toISOString());
    expect("2024-12-03T08:00:00.000Z").toBe(new Date("2024-12-3").toISOString());
    expect("2024-07-07T07:00:00.000Z").toBe(new Date("2024 7 7").toISOString());
    expect("2024-07-07T07:00:00.000Z").toBe(new Date("2024.7.7").toISOString());
    expect("2024-12-03T08:00:00.000Z").toBe(new Date("2024.12.3").toISOString());
    expect("2032-10-11T07:00:00.000Z").toBe(new Date("32 10 11").toISOString());
    expect("2037-10-11T07:00:00.000Z").toBe(new Date("37 10 11").toISOString());
    expect("1964-10-11T07:00:00.000Z").toBe(new Date("64 10 11").toISOString());
    expect("1999-10-11T07:00:00.000Z").toBe(new Date("99 10 11").toISOString());
    expect("0898-10-11T08:12:28.000Z").toBe(new Date("898 10 11").toISOString());

    expect("2011-12-24T08:00:00.000Z").toBe(new Date("+12/-24/+2011").toISOString()); // Permissive punctuation

    expect("2000-02-03T08:00:00.000Z").toBe(new Date("0 2 3").toISOString()); // First zero is a year number.
    expect("2003-10-02T07:00:00.000Z").toBe(new Date("10 2 3").toISOString()); // MDY

    expect(Date.parse("13 10 11")).toBeNaN(); // First number between 13-31 fails
    expect(Date.parse("20 10 11")).toBeNaN();
    expect(Date.parse("30 10 11")).toBeNaN();
    expect(Date.parse("31 10 11")).toBeNaN();

    expect(Date.parse("40 18 10")).toBeNaN(); // YDM fails

    expect(Date.parse("7 8 9 Feb")).toBeNaN(); // Introducing an explicit month fails (too many numbers).
    expect(Date.parse("7 8 Feb 9")).toBeNaN();
    expect(Date.parse("7 Feb 8 9")).toBeNaN();
    expect(Date.parse("Feb 7 8 9")).toBeNaN();

    expect(Date.parse("0 0")).toBeNaN(); // YM with M=0
    expect(Date.parse("2000 0")).toBeNaN();
    expect(Date.parse("0 0 0")).toBeNaN(); // YMD
    expect(Date.parse("0 0 1")).toBeNaN();
    expect(Date.parse("0 1 0")).toBeNaN();
    expect(Date.parse("10 0 0")).toBeNaN(); // MDY
    expect(Date.parse("10 0 1")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("ambiguous date: month + number", () => {
    // Firefox fails every time; Chrome is [arguably] inconsistent. We are [arguably] correct every time.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2024-12-01T08:00:00.000Z").toBe(new Date("Dec 2024").toISOString());
    expect("1970-10-01T07:00:00.000Z").toBe(new Date("Octebor 1970").toISOString()); // Only the first three letters of the month count.
    expect("1970-02-01T08:00:00.000Z").toBe(new Date("FebluAli-70").toISOString()); // Normal 2-digit year guessing applies (<50 -- +2000; >=50 +1900)
    expect("1970-02-01T08:00:00.000Z").toBe(new Date("February 70").toISOString());
    expect("2022-05-01T07:00:00.000Z").toBe(new Date("mayberry-22").toISOString());
    expect("2011-06-01T07:00:00.000Z").toBe(new Date("junter-11").toISOString());
    expect("-022024-12-01T08:12:28.000Z").toBe(new Date("-022024 December").toISOString());
    expect("2024-12-01T08:00:00.000Z").toBe(new Date("2024 December").toISOString());
    expect("1989-02-01T08:00:00.000Z").toBe(new Date("89-FebluAli").toISOString());
    expect("2024-09-01T07:00:00.000Z").toBe(new Date("24 SePaRaTiNG").toISOString());
    expect("1978-08-01T07:00:00.000Z").toBe(new Date("78 auger").toISOString());
    expect("1971-12-01T08:00:00.000Z").toBe(new Date("71 decimate").toISOString());

    // Chrome interprets numbers 1-31 as days in the given month in 2001. Weird. We interepret everything as a year.
    expect("2007-02-01T08:00:00.000Z").toBe(new Date("7.FebluAli").toISOString());
    expect("2021-02-01T08:00:00.000Z").toBe(new Date("21.FebluAli").toISOString());
    expect("2031-02-01T08:00:00.000Z").toBe(new Date("31.FebluAli").toISOString());
    expect("2032-02-01T08:00:00.000Z").toBe(new Date("32.FebluAli").toISOString());

    setTimeZone(originalTimeZone);
});

test("ambiguous date: month + 2 numbers", () => {
    // Compatible with Firefox and Chrome.
    const originalTimeZone = setTimeZone("America/Vancouver");

    // Unambiguous is obvious:
    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Dec 5 2024").toISOString());
    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Dec 2024 5").toISOString());
    expect("2017-06-20T07:00:00.000Z").toBe(new Date("20 Jun 2017").toISOString());
    expect("2017-06-20T07:00:00.000Z").toBe(new Date("2017 Jun 20").toISOString());
    expect("1986-04-18T08:00:00.000Z").toBe(new Date("18 1986 Apr").toISOString());
    expect("1986-04-18T08:00:00.000Z").toBe(new Date("1986 18 Apr").toISOString());

    expect("2006-12-05T08:00:00.000Z").toBe(new Date("5 dec 6").toISOString()); // Ambiguous defaults to "D month Y".
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("5 dec 32").toISOString());
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("32 dec 5").toISOString()); // High first number is year.

    expect("2006-12-05T08:00:00.000Z").toBe(new Date("dec 5 6").toISOString()); // Ambiguous defaults to "month D Y".
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("dec 5 32").toISOString());
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("dec 32 5").toISOString()); // High first number is year.

    expect("2006-12-05T08:00:00.000Z").toBe(new Date("5 6 dec").toISOString()); // Ambiguous defaults to "D Y month"
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("5 32 dec").toISOString());
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("32 5 dec").toISOString()); // High first number is year.

    // Only first 3 letters of the month name matter.
    // All kinds of punctuation is accepted.
    expect("1999-01-20T08:00:00.000Z").toBe(new Date("20-janitors-1999").toISOString());
    expect("1999-06-20T07:00:00.000Z").toBe(new Date("20.Junuary, 1999").toISOString());
    expect("2004-06-23T07:00:00.000Z").toBe(new Date("Junuary, 23 2004").toISOString());
    expect("2004-04-11T07:00:00.000Z").toBe(new Date("2004/Apron/11").toISOString());
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("Decision/5-32").toISOString());
    expect("2032-12-05T08:00:00.000Z").toBe(new Date("DeciMates 32 5").toISOString());

    expect(Date.parse("20+Junuary, 1999")).toBeNaN(); // Invalid punctuation.
    expect(Date.parse("33 34 dec")).toBeNaN(); // Neither of the numbers can be a day.
    expect(Date.parse("33 dec 34")).toBeNaN();
    expect(Date.parse("dec 33 34")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("time with colon", () => {
    // Compatible with Chrome and Firefox.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2048-12-24T09:23:00.000Z").toBe(new Date("12/24/2048 1:23").toISOString());
    expect("2048-12-24T09:23:45.000Z").toBe(new Date("12/24/2048 1:23:45").toISOString());
    expect("2048-12-24T09:23:45.600Z").toBe(new Date("12/24/2048 1:23:45.6").toISOString());
    expect("2048-12-24T09:23:45.670Z").toBe(new Date("12/24/2048 1:23:45.67").toISOString());
    expect("2048-12-24T09:23:45.678Z").toBe(new Date("12/24/2048 1:23:45.678").toISOString());
    expect("2048-12-24T09:23:45.678Z").toBe(new Date("12/24/2048 1:23:45.6789").toISOString()); // Truncated to milliseconds.
    expect("2048-12-24T09:23:45.678Z").toBe(new Date("12/24/2048 1:23:45.678999999").toISOString());
    // An insane number of digits.
    expect("2048-12-24T09:23:45.678Z").toBe(
        new Date(
            "12/24/2048 1:23:45.67899999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
        ).toISOString()
    );

    // 24:00
    expect("2048-12-24T08:00:00.000Z").toBe(new Date("12/24/2048 00:00").toISOString());
    expect("2048-12-25T08:00:00.000Z").toBe(new Date("12/24/2048 24:00").toISOString()); // Time 24:00 is the next day
    expect("2048-12-25T08:00:00.000Z").toBe(new Date("12/24/2048 24:00:00").toISOString());

    // Partial date.
    expect("2048-01-01T20:34:00.000Z").toBe(new Date("2048 12:34").toISOString());
    expect("2048-09-01T19:34:00.000Z").toBe(new Date("Sep 2048 12:34").toISOString());
    expect("2048-07-01T19:34:00.000Z").toBe(new Date("2048 Julie 12:34").toISOString());
    expect("1999-07-01T19:34:00.000Z").toBe(new Date("99 Julie 12:34").toISOString());
    expect("2012-07-01T19:34:00.000Z").toBe(new Date("12 Julie 12:34").toISOString());
    expect("2000-01-01T20:23:00.000Z").toBe(new Date("2000-12:23").toISOString()); // Recover from unexpected time in ISO8601 date format.

    expect(Date.parse("12/24/2048 1")).toBeNaN(); // Bare hours fail. At a minimum, time needs a colon ':'.
    expect(Date.parse("12/24/2048 01")).toBeNaN();
    expect(Date.parse("12/24/2048 1:2")).toBeNaN(); // Two digits needed for minutes.
    expect(Date.parse("12/24/2048 1:23:4")).toBeNaN(); // Two digits needed for seconds

    expect(Date.parse("12/24/2048 24:01")).toBeNaN(); // '24' hour needs 0 min 0 sec
    expect(Date.parse("12/24/2048 24:01:00")).toBeNaN();
    expect(Date.parse("12/24/2048 24:00:01")).toBeNaN();

    expect(Date.parse("12/24/2048 44:12")).toBeNaN(); // Hour must be at most 24
    expect(Date.parse("12/24/2048 12:66")).toBeNaN(); // Minutes must be at most 59
    expect(Date.parse("12/24/2048 12:34:66")).toBeNaN(); // Seconds must be at most 59
    expect(Date.parse("2048 11 12:34")).toBeNaN(); // guessing date from 2-digits still fails

    expect(Date.parse("2000-09-12:23")).toBeNaN(); // Firefox and Chrome fail. So do we.
    expect(Date.parse("2000-09-08-12:23")).toBeNaN(); // Firefox parses correctly. Chrome and us fail.

    // Time before date.
    expect("2048-12-24T09:23:00.000Z").toBe(new Date("1:23 12/24/2048").toISOString());
    expect("2048-12-24T21:23:00.000Z").toBe(new Date("1:23PM 12/24/2048").toISOString());
    expect("2048-12-24T09:23:45.000Z").toBe(new Date("1:23:45 12/24/2048").toISOString());
    expect("2048-12-24T09:23:45.678Z").toBe(new Date("1:23:45.678 12/24/2048").toISOString());
    expect("2048-12-24T09:23:45.678Z").toBe(new Date("1:23:45.678999 12/24/2048").toISOString());
    expect("2048-12-01T09:23:00.000Z").toBe(new Date("1:23 Dec 2048").toISOString());
    expect("2048-01-01T09:23:00.000Z").toBe(new Date("1:23 2048").toISOString());

    expect(Date.parse("1:23 12/2048")).toBeNaN(); // Two numbers cannot be converted to a date..

    setTimeZone(originalTimeZone);
});

test("am/pm", () => {
    // Compatible with Chrome (mostly) and Firefox.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2019-05-12T08:24:00.000Z").toBe(new Date("5/12/2019 1:24AM").toISOString()); // Chrome needs space between time and AM/PM
    expect("2019-05-12T08:24:00.000Z").toBe(new Date("5/12/2019 1:24 Am").toISOString()); // Space and capitalization.
    expect("2019-05-12T08:24:38.000Z").toBe(new Date("5/12/2019 1:24:38 Am").toISOString());
    expect("2019-05-12T20:24:38.123Z").toBe(new Date("5/12/2019 1:24:38.123pM").toISOString());

    // Time before date.
    expect("2048-12-24T09:23:00.000Z").toBe(new Date("1:23AM 12/24/2048").toISOString()); // Chrome fails on AM/PM time before date. Firefox and us support it.
    expect("2048-12-24T09:23:45.000Z").toBe(new Date("1:23:45AM 12/24/2048").toISOString());
    expect("2048-12-24T09:23:45.678Z").toBe(new Date("1:23:45.678AM 12/24/2048").toISOString());

    expect("2019-05-12T07:34:00.000Z").toBe(new Date("5/12/2019 00:34 AM").toISOString()); // Midnight
    expect("2019-05-12T07:34:00.000Z").toBe(new Date("5/12/2019 12:34 AM").toISOString());
    expect("2019-05-12T19:34:00.000Z").toBe(new Date("5/12/2019 00:34 PM").toISOString()); // Noon
    expect("2019-05-12T19:34:00.000Z").toBe(new Date("5/12/2019 12:34 PM").toISOString());

    // Absurdly many digits
    expect("2025-02-11T21:02:03.123Z").toBe(
        new Date(
            "2/11/25 1:02:03.123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890 PM"
        ).toISOString()
    );

    expect(Date.parse("8/15/1999 3AM")).toBeNaN(); // Needs time with colon (at least HH:MM)
    expect(Date.parse("8/15/1999 14:33AM")).toBeNaN(); // Hour less than or equal to 12

    setTimeZone(originalTimeZone);
});

test("timezone offset", () => {
    // Mostly compatible with Chrome and Firefox.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2019-07-18T11:22:00.000Z").toBe(new Date("07/18/2019 11:22Z").toISOString());
    expect("2019-07-18T11:22:00.000Z").toBe(new Date("07/18/2019 11:22GMT").toISOString()); // Firefox and Chrome expect a space between time and GMT/UTC. So do not    q.
    expect("2019-07-18T11:22:00.000Z").toBe(new Date("07/18/2019 11:22UTC").toISOString());
    expect("2019-07-18T11:22:00.000Z").toBe(new Date("07/18/2019 11:22 z").toISOString()); // Space and capitalization
    expect("2019-07-18T11:22:00.000Z").toBe(new Date("07/18/2019 11:22 UTC").toISOString());
    expect("2019-07-18T11:22:00.000Z").toBe(new Date("07/18/2019 11:22 GMT").toISOString());

    expect("2019-07-18T08:22:00.000Z").toBe(new Date("07/18/2019 11:22 +03").toISOString());
    expect("2019-07-18T08:22:00.000Z").toBe(new Date("07/18/2019 11:22 +3").toISOString()); // 1-digit hour
    expect("2019-07-18T08:22:00.000Z").toBe(new Date("07/18/2019 11:22+3").toISOString()); // No space
    expect("2019-07-18T08:01:00.000Z").toBe(new Date("07/18/2019 11:22 +03:21").toISOString());
    expect("2019-07-18T08:01:00.000Z").toBe(new Date("07/18/2019 11:22 +3:21").toISOString());

    expect("2019-07-18T08:00:51.000Z").toBe(new Date("07/18/2019 11:22 +03:21:09").toISOString()); // Chrome and Firefox do not support seconds in timezone offset
    expect("2019-07-18T08:00:50.877Z").toBe(
        new Date("07/18/2019 11:22 +03:21:09.123").toISOString()
    );
    expect("2019-07-18T08:00:50.877Z").toBe(
        new Date("07/18/2019 11:22 +03:21:09.12345678").toISOString()
    ); // Truncate to milliseconds

    expect("2019-07-18T08:22:00.000Z").toBe(new Date("07/18/2019 11:22 GMT+03").toISOString());
    expect("2019-07-18T08:22:00.000Z").toBe(new Date("07/18/2019 11:22 UTC+03").toISOString());
    expect("2019-07-18T08:22:00.000Z").toBe(new Date("07/18/2019 11:22 Z+03").toISOString());

    expect(Date.parse("3/8/89 +01:23")).toBeNaN(); // Firefox and Chrome need GMT/Z/UTC before timezone offset when time is not specified. So do we.

    // Military timezone offset
    expect("2025-02-11T00:02:00.000Z").toBe(new Date("2/11/25 1:02+1").toISOString()); // Same as +01:00.
    expect("2025-02-11T00:02:00.000Z").toBe(new Date("2/11/25 1:02+01").toISOString());
    expect("2025-02-11T00:50:00.000Z").toBe(new Date("2/11/25 1:02+012").toISOString()); // Same as +00:12.
    expect("2025-02-10T23:39:00.000Z").toBe(new Date("2/11/25 1:02+0123").toISOString());
    expect("2025-02-11T00:49:26.000Z").toBe(new Date("2/11/25 1:02+01234").toISOString()); // Same as +00:12:34 (below).
    // Chrome does not support 5 or 6 digit timezone offset. Firefox ignores seconds. We support it.
    expect("2025-02-10T23:38:15.000Z").toBe(new Date("2/11/25 1:02+012345").toISOString());

    // Date-only timezone offset needs GMT/UTC/Z
    expect("1989-03-08T01:00:00.000Z").toBe(new Date("3/8/89 GMT-1").toISOString());
    expect("1989-03-08T12:00:00.000Z").toBe(new Date("3/8/89 GMT-12").toISOString());
    expect("1989-03-08T01:23:00.000Z").toBe(new Date("3/8/89 GMT-123").toISOString());
    expect("1989-03-08T12:34:00.000Z").toBe(new Date("3/8/89 GMT-1234").toISOString());
    expect("1989-03-08T01:00:00.000Z").toBe(new Date("3/8/89 UTC-1").toISOString());
    expect("1989-03-08T01:00:00.000Z").toBe(new Date("3/8/89 Z-1").toISOString());

    // 6-digit signed year is not timezone offset
    expect("2025-02-11T08:00:00.000Z").toBe(new Date("Feb 11 +002025").toISOString());
    expect("2025-02-11T08:00:00.000Z").toBe(new Date("2/11 +002025").toISOString());

    expect(Date.parse("2/11 GMT+002025")).toBeNaN(); // GMT introduces a 6-digit "military" time offset, but we cannot guess date from 2 two numbers.
    expect(Date.parse("2/11/25 +1")).toBeNaN(); // Date-only military timezone needs GMT/UTC/Z.
    expect(Date.parse("2/11/25 +12")).toBeNaN();
    expect(Date.parse("2/11/25 +123")).toBeNaN();
    expect(Date.parse("2/11/25 +1234")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("us timezones", () => {
    const originalTimeZone = setTimeZone("America/Vancouver");

    // Compatible with Chrome and Firefox
    // US mainland time zones are supported
    expect("2024-12-08T23:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 PST").toISOString());
    expect("2024-12-08T23:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 pSt").toISOString()); // Capitalization does not matter.
    expect("2024-12-08T22:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 MST").toISOString());
    expect("2024-12-08T21:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 CST").toISOString());
    expect("2024-12-08T20:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 EST").toISOString());
    expect("2024-12-08T22:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 PDT").toISOString()); // Daylight can be explicitly on any date (arguably incorrect).
    expect("2024-12-08T21:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 MDT").toISOString());
    expect("2024-12-08T20:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 CDT").toISOString());
    expect("2024-12-08T19:30:00.000Z").toBe(new Date("8-Dec-2024 15:30:00 EDT").toISOString());

    // Timezone (or GMT) indicator can be placed anywhere
    expect("2025-02-23T08:00:00.000Z").toBe(new Date("EST 23 feb 2025").toISOString()); // Timezone name before time of the start of the date is ignored.

    expect("2025-02-23T17:34:00.000Z").toBe(new Date("23 EST feb 2025 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 GMT feb 2025 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 UTC feb 2025 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 Z feb 2025 12:34").toISOString());

    expect("2025-02-23T17:34:00.000Z").toBe(new Date("23 feb EST 2025 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb GMT 2025 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb UTC 2025 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb Z 2025 12:34").toISOString());

    expect("2025-02-23T17:34:00.000Z").toBe(new Date("23 feb 2025 EST 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb 2025 GMT 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb 2025 UTC 12:34").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb 2025 Z 12:34").toISOString());

    expect("2025-02-23T17:34:00.000Z").toBe(new Date("23 feb 2025 12:34 EST").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb 2025 12:34 GMT").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb 2025 12:34 UTC").toISOString());
    expect("2025-02-23T12:34:00.000Z").toBe(new Date("23 feb 2025 12:34 Z").toISOString());

    expect("2025-02-23T18:34:00.000Z").toBe(
        new Date("23 EST feb PDT 2025 CST 12:34").toISOString()
    ); // For multiple occurrences, the last one wins
    expect("2022-02-12T20:34:00.000Z").toBe(new Date("2/12/22 12:34 +0100 PST").toISOString()); // In timezone name wins after timezone offset.

    expect(Date.parse("12-Dec-2024 15:30:00 PT")).toBeNaN(); // Non-qualified timezone names ("PT"="Pacific Time"; no S/D) are not recognized

    expect(Date.parse("12-Dec-2024 15:30:00 HST")).toBeNaN(); // Hawaii and Alaska are not recognized
    expect(Date.parse("12-Dec-2024 15:30:00 AKST")).toBeNaN();

    expect(Date.parse("12-Dec-2024 15:30:00 AST")).toBeNaN(); // Canadian Atlantic Standard Time (AST) and Newfoundland Standard Time are not recognized
    expect(Date.parse("12-Dec-2024 15:30:00 NST")).toBeNaN();

    // Chrome overwrites the timezone name. Firefox and us fail on timezone offset after time zone name.
    expect(Date.parse("2/12/22 12:34 PST +0100")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("weekdays", () => {
    // Weekday names are ignored. Compatible with Chrome and Firefox.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Thu Dec 5 2024").toISOString());
    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Thursday Dec 5 2024").toISOString());
    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Thurnip Dec 5 2024").toISOString());
    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Mon Dec 5 2024").toISOString()); // Incorrect weekday is ignored.
    expect("2024-12-05T08:00:00.000Z").toBe(new Date("Theater Dec 5 2024").toISOString()); // Anything before the first number is ignored.
    expect("2024-12-05T08:00:00.000Z").toBe(
        new Date("Mon Tue Wed Dec Thu YMCA 5 2024").toISOString()
    );

    expect(Date.parse("Mon Dec Tue 5 Wed 2024")).toBeNaN(); // Weekday names (or any other word) fail after the first number
    expect(Date.parse("Mon Dec Tue 5 Wed 2024 Thu")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("timezone name", () => {
    // Compatible with Chrome and Firefox. Anything that is not a closing bracket following an open bracket is ignored.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2020-12-05T20:23:00.000Z").toBe(
        new Date("Thu Dec 5 2020 12:23 -0800 (America/Vancouver)").toISOString()
    );
    expect("2020-12-05T20:23:00.000Z").toBe(
        new Date("Thu Dec 5 2020 12:23 -0800 (America/Chicago)").toISOString()
    ); // Incorrect time zone name.
    expect("2020-12-05T20:23:00.000Z").toBe(
        new Date("Thu Dec 5 2020 12:23 (America/Chicago)").toISOString()
    ); // Missing timezone offset makes it local time. Timezone name is ignored.
    expect("2020-12-05T20:23:00.000Z").toBe(
        new Date(
            "Thu Dec 5 2020 12:23 -0800 (Whatever alpha 123 numerics or punctuation $# ({}[] works )"
        ).toISOString()
    );

    expect(Date.parse("Mon Dec Tue 5 Wed 2024 (Cannot close a bracket ) twice)")).toBeNaN();
    expect(Date.parse("Mon Dec Tue 5 Wed 2024 (Nothing after final bracket).")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("garbage", () => {
    // Mostly compatible with Firefox. Chrome has more permissive punctuation but more restrictive syntax.
    const originalTimeZone = setTimeZone("America/Vancouver");

    expect("2204-01-20T02:32:00.000Z").toBe(
        // Garbage (words and punctuation) are accepted at the start.
        new Date(
            "--sapdoaisfdm -,./ /dev/null world, .hello 2204 Jan 20 10:32 GMT+08:00" // Firefox and us accept -,./ Chrome is more permissive.
        ).toISOString()
    );

    // Accept punctuation at any point before time.
    // Firefox fails in some conditions before time.
    // Chrome is finnicky on interior punctuation.
    expect("2204-01-20T02:32:00.000Z").toBe(
        new Date("2204 //. -/, Jan ./- 20 ..- 10:32 GMT+08:00").toISOString()
    );

    // Chrome does not accept punctuation after time. Firefox accepts some. We accept punctuation.
    expect("2204-01-29T18:30:00.000Z").toBe(new Date("2204 Jan 29 10:30 /--/").toISOString());
    expect("2204-01-29T23:04:00.000Z").toBe(new Date("2204 Jan 29 10:30 /--/ -1234").toISOString());

    expect(Date.parse("2204 Jan 29 10:30. /-/")).toBeNaN(); // But we do not accept a dot after time.
    expect(Date.parse("2204 Jan 29 10:30:28.")).toBeNaN();
    expect(Date.parse("2204 twice Jan 29")).toBeNaN(); // No words after first number.
    expect(Date.parse("+- 2204 Jan 29")).toBeNaN(); // '+' accepted by Chrome rejected by Firefox in some weird conditions. We reject it always.

    expect(Date.parse("2204 + Jan 29")).toBeNaN();
    expect(Date.parse("2204 Jan + 29")).toBeNaN();
    expect(Date.parse("2204 + Jan 29")).toBeNaN();

    setTimeZone(originalTimeZone);
});

test("multiple month names", () => {
    // Compatible with Chrome and Firefox.
    const originalTimeZone = setTimeZone("America/Vancouver");

    // Multiple month names are accepted. Last one wins.
    expect("1981-03-23T22:56:00.000Z").toBe(
        new Date("March 23, 1981 14:56 GMT-08:00").toISOString()
    );
    expect("1981-04-23T22:56:00.000Z").toBe(
        new Date("March 23, Apr 1981 14:56 GMT-08:00").toISOString()
    );
    expect("1981-05-23T22:56:00.000Z").toBe(
        new Date("March 23, Apron 1981 mAY 14:56 GMT-08:00").toISOString()
    );
    expect("1981-06-23T22:56:00.000Z").toBe(
        new Date("March 23, Apron 1981 mAY 14:56 junuary GMT-08:00").toISOString()
    );
    expect("1981-07-23T22:56:00.000Z").toBe(
        new Date("March 23, Apron 1981 mAY 14:56 junuary GMT-08:00 Julie").toISOString()
    );

    setTimeZone(originalTimeZone);
});
