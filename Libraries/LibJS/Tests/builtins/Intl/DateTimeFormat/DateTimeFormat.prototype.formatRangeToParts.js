describe("errors", () => {
    test("called on non-DateTimeFormat object", () => {
        expect(() => {
            Intl.DateTimeFormat.prototype.formatRangeToParts(1, 2);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.DateTimeFormat");
    });

    test("called with undefined values", () => {
        expect(() => {
            Intl.DateTimeFormat().formatRangeToParts();
        }).toThrowWithMessage(TypeError, "startDate is undefined");

        expect(() => {
            Intl.DateTimeFormat().formatRangeToParts(1);
        }).toThrowWithMessage(TypeError, "endDate is undefined");
    });

    test("called with values that cannot be converted to numbers", () => {
        expect(() => {
            Intl.DateTimeFormat().formatRangeToParts(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            Intl.DateTimeFormat().formatRangeToParts(1n, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("time value cannot be clipped", () => {
        [NaN, -8.65e15, 8.65e15].forEach(d => {
            expect(() => {
                Intl.DateTimeFormat().formatRangeToParts(d, 1);
            }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

            expect(() => {
                Intl.DateTimeFormat().formatRangeToParts(1, d);
            }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");
        });
    });

    test("Temporal object must have same calendar", () => {
        const formatter = new Intl.DateTimeFormat([], { calendar: "iso8601" });

        expect(() => {
            const plainDate = new Temporal.PlainDate(1972, 1, 1, "gregory");
            formatter.formatRangeToParts(plainDate, plainDate);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDate with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            const plainYearMonth = new Temporal.PlainYearMonth(1972, 1, "gregory");
            formatter.formatRangeToParts(plainYearMonth, plainYearMonth);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainYearMonth with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            const plainMonthDay = new Temporal.PlainMonthDay(1, 1, "gregory");
            formatter.formatRangeToParts(plainMonthDay, plainMonthDay);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainMonthDay with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            const plainDateTime = new Temporal.PlainDateTime(
                1972,
                1,
                1,
                8,
                45,
                56,
                123,
                345,
                789,
                "gregory"
            );
            formatter.formatRangeToParts(plainDateTime, plainDateTime);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDateTime with calendar 'gregory' in locale with calendar 'iso8601'"
        );
    });

    test("cannot format Temporal.ZonedDateTime", () => {
        expect(() => {
            const zonedDateTime = new Temporal.ZonedDateTime(0n, "UTC");
            new Intl.DateTimeFormat().formatRangeToParts(zonedDateTime, zonedDateTime);
        }).toThrowWithMessage(
            TypeError,
            "Cannot format Temporal.ZonedDateTime, use Temporal.ZonedDateTime.prototype.toLocaleString"
        );
    });

    test("cannot mix Temporal object types", () => {
        expect(() => {
            const plainDate = new Temporal.PlainDate(1972, 1, 1, "gregory");
            new Intl.DateTimeFormat().formatRangeToParts(plainDate, 0);
        }).toThrowWithMessage(
            TypeError,
            "Cannot format a date-time range with different date-time types"
        );

        expect(() => {
            const plainYearMonth = new Temporal.PlainYearMonth(1972, 1, "gregory");
            const plainMonthDay = new Temporal.PlainMonthDay(1, 1, "gregory");
            new Intl.DateTimeFormat().formatRangeToParts(plainYearMonth, plainMonthDay);
        }).toThrowWithMessage(
            TypeError,
            "Cannot format a date-time range with different date-time types"
        );
    });

    test("Temporal fields must overlap formatter", () => {
        const yearFormatter = new Intl.DateTimeFormat([], { year: "numeric", calendar: "iso8601" });
        const plainMonthDay = new Temporal.PlainMonthDay(1, 1);

        const dayFormatter = new Intl.DateTimeFormat([], { day: "numeric", calendar: "iso8601" });
        const plainYearMonth = new Temporal.PlainYearMonth(1972, 1);

        expect(() => {
            yearFormatter.formatRangeToParts(plainMonthDay, plainMonthDay);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainMonthDay");

        expect(() => {
            dayFormatter.formatRangeToParts(plainYearMonth, plainYearMonth);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainYearMonth");
    });
});

const d0 = Date.UTC(1989, 0, 23, 7, 8, 9, 45);
const d1 = Date.UTC(2021, 11, 7, 17, 40, 50, 456);

describe("equal dates are squashed", () => {
    test("with date fields", () => {
        const en = new Intl.DateTimeFormat("en", {
            year: "numeric",
            month: "long",
            day: "2-digit",
            timeZone: "UTC",
        });
        expect(en.formatRangeToParts(d0, d0)).toEqual([
            { type: "month", value: "January", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "day", value: "23", source: "shared" },
            { type: "literal", value: ", ", source: "shared" },
            { type: "year", value: "1989", source: "shared" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", {
            year: "numeric",
            month: "long",
            day: "2-digit",
            timeZone: "UTC",
        });
        expect(ja.formatRangeToParts(d0, d0)).toEqual([
            { type: "year", value: "1989", source: "shared" },
            { type: "literal", value: "年", source: "shared" },
            { type: "month", value: "1", source: "shared" },
            { type: "literal", value: "月", source: "shared" },
            { type: "day", value: "23", source: "shared" },
            { type: "literal", value: "日", source: "shared" },
        ]);
    });

    test("with time fields", () => {
        const en = new Intl.DateTimeFormat("en", {
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(en.formatRangeToParts(d0, d0)).toEqual([
            { type: "hour", value: "7", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "minute", value: "08", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "second", value: "09", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "dayPeriod", value: "AM", source: "shared" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", {
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(ja.formatRangeToParts(d0, d0)).toEqual([
            { type: "hour", value: "7", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "minute", value: "08", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "second", value: "09", source: "shared" },
        ]);
    });

    test("with mixed fields", () => {
        const en = new Intl.DateTimeFormat("en", {
            year: "numeric",
            month: "long",
            day: "2-digit",
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(en.formatRangeToParts(d0, d0)).toEqual([
            { type: "month", value: "January", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "day", value: "23", source: "shared" },
            { type: "literal", value: ", ", source: "shared" },
            { type: "year", value: "1989", source: "shared" },
            { type: "literal", value: " at ", source: "shared" },
            { type: "hour", value: "7", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "minute", value: "08", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "second", value: "09", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "dayPeriod", value: "AM", source: "shared" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", {
            year: "numeric",
            month: "long",
            day: "2-digit",
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(ja.formatRangeToParts(d0, d0)).toEqual([
            { type: "year", value: "1989", source: "shared" },
            { type: "literal", value: "年", source: "shared" },
            { type: "month", value: "1", source: "shared" },
            { type: "literal", value: "月", source: "shared" },
            { type: "day", value: "23", source: "shared" },
            { type: "literal", value: "日 ", source: "shared" },
            { type: "hour", value: "7", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "minute", value: "08", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "second", value: "09", source: "shared" },
        ]);
    });

    test("with date/time style fields", () => {
        const en = new Intl.DateTimeFormat("en", {
            dateStyle: "full",
            timeStyle: "medium",
            timeZone: "UTC",
        });
        expect(en.formatRangeToParts(d0, d0)).toEqual([
            { type: "weekday", value: "Monday", source: "shared" },
            { type: "literal", value: ", ", source: "shared" },
            { type: "month", value: "January", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "day", value: "23", source: "shared" },
            { type: "literal", value: ", ", source: "shared" },
            { type: "year", value: "1989", source: "shared" },
            { type: "literal", value: " at ", source: "shared" },
            { type: "hour", value: "7", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "minute", value: "08", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "second", value: "09", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "dayPeriod", value: "AM", source: "shared" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", {
            dateStyle: "full",
            timeStyle: "medium",
            timeZone: "UTC",
        });
        expect(ja.formatRangeToParts(d0, d0)).toEqual([
            { type: "year", value: "1989", source: "shared" },
            { type: "literal", value: "年", source: "shared" },
            { type: "month", value: "1", source: "shared" },
            { type: "literal", value: "月", source: "shared" },
            { type: "day", value: "23", source: "shared" },
            { type: "literal", value: "日", source: "shared" },
            { type: "weekday", value: "月曜日", source: "shared" },
            { type: "literal", value: " ", source: "shared" },
            { type: "hour", value: "7", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "minute", value: "08", source: "shared" },
            { type: "literal", value: ":", source: "shared" },
            { type: "second", value: "09", source: "shared" },
        ]);
    });
});

describe("dateStyle", () => {
    test("full", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "full", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "weekday", value: "Monday", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "month", value: "January", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "weekday", value: "Tuesday", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "month", value: "December", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "full", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "(", source: "startRange" },
            { type: "weekday", value: "月曜日", source: "startRange" },
            { type: "literal", value: ")～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "07", source: "endRange" },
            { type: "literal", value: "(", source: "endRange" },
            { type: "weekday", value: "火曜日", source: "endRange" },
            { type: "literal", value: ")", source: "shared" },
        ]);
    });

    test("long", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "long", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "January", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "December", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "long", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "07", source: "endRange" },
        ]);
    });

    test("medium", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "medium", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "Jan", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "Dec", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "medium", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "07", source: "endRange" },
        ]);
    });

    test("short", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "short", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "year", value: "89", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "year", value: "21", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "short", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "07", source: "endRange" },
        ]);
    });

    test("dates in reverse order", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "full", timeZone: "UTC" });
        expect(en.formatRangeToParts(d1, d0)).toEqual([
            { type: "weekday", value: "Tuesday", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "month", value: "December", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "day", value: "7", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "year", value: "2021", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "weekday", value: "Monday", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "month", value: "January", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "day", value: "23", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "year", value: "1989", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "full", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d1, d0)).toEqual([
            { type: "year", value: "2021", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "12", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "07", source: "startRange" },
            { type: "literal", value: "(", source: "startRange" },
            { type: "weekday", value: "火曜日", source: "startRange" },
            { type: "literal", value: ")～", source: "shared" },
            { type: "year", value: "1989", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "01", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "23", source: "endRange" },
            { type: "literal", value: "(", source: "endRange" },
            { type: "weekday", value: "月曜日", source: "endRange" },
            { type: "literal", value: ")", source: "shared" },
        ]);
    });
});

describe("timeStyle", () => {
    // FIXME: These results should include the date, even though it isn't requested, because the start/end dates
    //        are more than just hours apart. See the FIXME in PartitionDateTimeRangePattern.
    test("full", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "full", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "09", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "dayPeriod", value: "AM", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "timeZoneName", value: "Coordinated Universal Time", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "hour", value: "5", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "50", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "dayPeriod", value: "PM", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "timeZoneName", value: "Coordinated Universal Time", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { timeStyle: "full", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: "時", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: "分", source: "startRange" },
            { type: "second", value: "09", source: "startRange" },
            { type: "literal", value: "秒 ", source: "startRange" },
            { type: "timeZoneName", value: "協定世界時", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "hour", value: "17", source: "endRange" },
            { type: "literal", value: "時", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: "分", source: "endRange" },
            { type: "second", value: "50", source: "endRange" },
            { type: "literal", value: "秒 ", source: "endRange" },
            { type: "timeZoneName", value: "協定世界時", source: "endRange" },
        ]);
    });

    test("long", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "long", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "09", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "dayPeriod", value: "AM", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "timeZoneName", value: "UTC", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "hour", value: "5", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "50", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "dayPeriod", value: "PM", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "timeZoneName", value: "UTC", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { timeStyle: "long", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "09", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "timeZoneName", value: "UTC", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "hour", value: "17", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "50", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "timeZoneName", value: "UTC", source: "endRange" },
        ]);
    });

    test("medium", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "medium", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "09", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "dayPeriod", value: "AM", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "hour", value: "5", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "50", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "dayPeriod", value: "PM", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { timeStyle: "medium", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "09", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "hour", value: "17", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "50", source: "endRange" },
        ]);
    });

    test("short", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "short", timeZone: "UTC" });
        expect(en.formatRangeToParts(d0, d1)).toEqual([
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "dayPeriod", value: "AM", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "hour", value: "5", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "dayPeriod", value: "PM", source: "endRange" },
        ]);

        const ja = new Intl.DateTimeFormat("ja", { timeStyle: "short", timeZone: "UTC" });
        expect(ja.formatRangeToParts(d0, d1)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "month", value: "1", source: "startRange" },
            { type: "literal", value: "/", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "hour", value: "7", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "08", source: "startRange" },
            { type: "literal", value: "～", source: "shared" },
            { type: "year", value: "2021", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "month", value: "12", source: "endRange" },
            { type: "literal", value: "/", source: "endRange" },
            { type: "day", value: "7", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "hour", value: "17", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "40", source: "endRange" },
        ]);
    });
});

describe("Temporal objects", () => {
    const formatter = new Intl.DateTimeFormat("en", {
        calendar: "iso8601",
        timeZone: "UTC",
    });

    test("Temporal.PlainDate", () => {
        const plainDate1 = new Temporal.PlainDate(1989, 1, 23);
        const plainDate2 = new Temporal.PlainDate(2024, 11, 27);
        expect(formatter.formatRangeToParts(plainDate1, plainDate2)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "-", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "-", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "year", value: "2024", source: "endRange" },
            { type: "literal", value: "-", source: "endRange" },
            { type: "month", value: "11", source: "endRange" },
            { type: "literal", value: "-", source: "endRange" },
            { type: "day", value: "27", source: "endRange" },
        ]);
    });

    test("Temporal.PlainYearMonth", () => {
        const plainYearMonth1 = new Temporal.PlainYearMonth(1989, 1);
        const plainYearMonth2 = new Temporal.PlainYearMonth(2024, 11);
        expect(formatter.formatRangeToParts(plainYearMonth1, plainYearMonth2)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "-", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "year", value: "2024", source: "endRange" },
            { type: "literal", value: "-", source: "endRange" },
            { type: "month", value: "11", source: "endRange" },
        ]);
    });

    test("Temporal.PlainMonthDay", () => {
        const plainMonthDay1 = new Temporal.PlainMonthDay(1, 23);
        const plainMonthDay2 = new Temporal.PlainMonthDay(11, 27);
        expect(formatter.formatRangeToParts(plainMonthDay1, plainMonthDay2)).toEqual([
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "-", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "month", value: "11", source: "endRange" },
            { type: "literal", value: "-", source: "endRange" },
            { type: "day", value: "27", source: "endRange" },
        ]);
    });

    test("Temporal.PlainTime", () => {
        const plainTime1 = new Temporal.PlainTime(8, 10, 51);
        const plainTime2 = new Temporal.PlainTime(20, 41, 9);
        expect(formatter.formatRangeToParts(plainTime1, plainTime2)).toEqual([
            { type: "hour", value: "8", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "10", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "51", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "dayPeriod", value: "AM", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "hour", value: "8", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "41", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "09", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "dayPeriod", value: "PM", source: "endRange" },
        ]);
    });

    test("Temporal.Instant", () => {
        const instant1 = new Temporal.Instant(601546251000000000n);
        const instant2 = new Temporal.Instant(1732740069000000000n);
        expect(formatter.formatRangeToParts(instant1, instant2)).toEqual([
            { type: "year", value: "1989", source: "startRange" },
            { type: "literal", value: "-", source: "startRange" },
            { type: "month", value: "01", source: "startRange" },
            { type: "literal", value: "-", source: "startRange" },
            { type: "day", value: "23", source: "startRange" },
            { type: "literal", value: ", ", source: "startRange" },
            { type: "hour", value: "8", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "minute", value: "10", source: "startRange" },
            { type: "literal", value: ":", source: "startRange" },
            { type: "second", value: "51", source: "startRange" },
            { type: "literal", value: " ", source: "startRange" },
            { type: "dayPeriod", value: "AM", source: "startRange" },
            { type: "literal", value: " – ", source: "shared" },
            { type: "year", value: "2024", source: "endRange" },
            { type: "literal", value: "-", source: "endRange" },
            { type: "month", value: "11", source: "endRange" },
            { type: "literal", value: "-", source: "endRange" },
            { type: "day", value: "27", source: "endRange" },
            { type: "literal", value: ", ", source: "endRange" },
            { type: "hour", value: "8", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "minute", value: "41", source: "endRange" },
            { type: "literal", value: ":", source: "endRange" },
            { type: "second", value: "09", source: "endRange" },
            { type: "literal", value: " ", source: "endRange" },
            { type: "dayPeriod", value: "PM", source: "endRange" },
        ]);
    });
});
