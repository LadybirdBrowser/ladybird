describe("errors", () => {
    test("called on non-DateTimeFormat object", () => {
        expect(() => {
            Intl.DateTimeFormat.prototype.formatRange(1, 2);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.DateTimeFormat");
    });

    test("called with undefined values", () => {
        expect(() => {
            Intl.DateTimeFormat().formatRange();
        }).toThrowWithMessage(TypeError, "startDate is undefined");

        expect(() => {
            Intl.DateTimeFormat().formatRange(1);
        }).toThrowWithMessage(TypeError, "endDate is undefined");
    });

    test("called with values that cannot be converted to numbers", () => {
        expect(() => {
            Intl.DateTimeFormat().formatRange(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            Intl.DateTimeFormat().formatRange(1n, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("time value cannot be clipped", () => {
        [NaN, -8.65e15, 8.65e15].forEach(d => {
            expect(() => {
                Intl.DateTimeFormat().formatRange(d, 1);
            }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

            expect(() => {
                Intl.DateTimeFormat().formatRange(1, d);
            }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");
        });
    });

    test("Temporal object must have same calendar", () => {
        const formatter = new Intl.DateTimeFormat([], { calendar: "iso8601" });

        expect(() => {
            const plainDate = new Temporal.PlainDate(1972, 1, 1, "gregory");
            formatter.formatRange(plainDate, plainDate);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDate with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            const plainYearMonth = new Temporal.PlainYearMonth(1972, 1, "gregory");
            formatter.formatRange(plainYearMonth, plainYearMonth);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainYearMonth with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            const plainMonthDay = new Temporal.PlainMonthDay(1, 1, "gregory");
            formatter.formatRange(plainMonthDay, plainMonthDay);
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
            formatter.formatRange(plainDateTime, plainDateTime);
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDateTime with calendar 'gregory' in locale with calendar 'iso8601'"
        );
    });

    test("cannot format Temporal.ZonedDateTime", () => {
        expect(() => {
            const zonedDateTime = new Temporal.ZonedDateTime(0n, "UTC");
            new Intl.DateTimeFormat().formatRange(zonedDateTime, zonedDateTime);
        }).toThrowWithMessage(
            TypeError,
            "Cannot format Temporal.ZonedDateTime, use Temporal.ZonedDateTime.prototype.toLocaleString"
        );
    });

    test("cannot mix Temporal object types", () => {
        expect(() => {
            const plainDate = new Temporal.PlainDate(1972, 1, 1, "gregory");
            new Intl.DateTimeFormat().formatRange(plainDate, 0);
        }).toThrowWithMessage(
            TypeError,
            "Cannot format a date-time range with different date-time types"
        );

        expect(() => {
            const plainYearMonth = new Temporal.PlainYearMonth(1972, 1, "gregory");
            const plainMonthDay = new Temporal.PlainMonthDay(1, 1, "gregory");
            new Intl.DateTimeFormat().formatRange(plainYearMonth, plainMonthDay);
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
            yearFormatter.formatRange(plainMonthDay, plainMonthDay);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainMonthDay");

        expect(() => {
            dayFormatter.formatRange(plainYearMonth, plainYearMonth);
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
        expect(en.formatRange(d0, d0)).toBe("January 23, 1989");
        expect(en.formatRange(d1, d1)).toBe("December 07, 2021");

        const ja = new Intl.DateTimeFormat("ja", {
            year: "numeric",
            month: "long",
            day: "2-digit",
            timeZone: "UTC",
        });
        expect(ja.formatRange(d0, d0)).toBe("1989年1月23日");
        expect(ja.formatRange(d1, d1)).toBe("2021年12月07日");
    });

    test("with time fields", () => {
        const en = new Intl.DateTimeFormat("en", {
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(en.formatRange(d0, d0)).toBe("7:08:09 AM");
        expect(en.formatRange(d1, d1)).toBe("5:40:50 PM");

        const ja = new Intl.DateTimeFormat("ja", {
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(ja.formatRange(d0, d0)).toBe("7:08:09");
        expect(ja.formatRange(d1, d1)).toBe("17:40:50");
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
        expect(en.formatRange(d0, d0)).toBe("January 23, 1989 at 7:08:09 AM");
        expect(en.formatRange(d1, d1)).toBe("December 07, 2021 at 5:40:50 PM");

        const ja = new Intl.DateTimeFormat("ja", {
            year: "numeric",
            month: "long",
            day: "2-digit",
            hour: "numeric",
            minute: "2-digit",
            second: "2-digit",
            timeZone: "UTC",
        });
        expect(ja.formatRange(d0, d0)).toBe("1989年1月23日 7:08:09");
        expect(ja.formatRange(d1, d1)).toBe("2021年12月07日 17:40:50");
    });

    test("with date/time style fields", () => {
        const en = new Intl.DateTimeFormat("en", {
            dateStyle: "full",
            timeStyle: "medium",
            timeZone: "UTC",
        });
        expect(en.formatRange(d0, d0)).toBe("Monday, January 23, 1989 at 7:08:09 AM");
        expect(en.formatRange(d1, d1)).toBe("Tuesday, December 7, 2021 at 5:40:50 PM");

        const ja = new Intl.DateTimeFormat("ja", {
            dateStyle: "full",
            timeStyle: "medium",
            timeZone: "UTC",
        });
        expect(ja.formatRange(d0, d0)).toBe("1989年1月23日月曜日 7:08:09");
        expect(ja.formatRange(d1, d1)).toBe("2021年12月7日火曜日 17:40:50");
    });
});

describe("dateStyle", () => {
    // prettier-ignore
    const data = [
        { date: "full", en: "Monday, January 23, 1989 – Tuesday, December 7, 2021", ja: "1989/01/23(月曜日)～2021/12/07(火曜日)" },
        { date: "long", en: "January 23, 1989 – December 7, 2021", ja: "1989/01/23～2021/12/07" },
        { date: "medium", en: "Jan 23, 1989 – Dec 7, 2021", ja: "1989/01/23～2021/12/07" },
        { date: "short", en: "1/23/89 – 12/7/21", ja: "1989/01/23～2021/12/07" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { dateStyle: d.date, timeZone: "UTC" });
            expect(en.formatRange(d0, d1)).toBe(d.en);

            // If this test is to be changed, take care to note the "long" style for the ja locale is an intentionally
            // chosen complex test case. The format pattern is "y年M月d日" and its skeleton is "yMd" - note that the
            // month field has a numeric style. However, the interval patterns that match the "yMd" skeleton are all
            // "y/MM/dd～y/MM/dd" - the month field there conflicts with a 2-digit style. This exercises the step in the
            // FormatDateTimePattern AO to choose the style from rangeFormatOptions instead of dateTimeFormat (step 15.f.i
            // as of when this test was written).
            const ja = new Intl.DateTimeFormat("ja", { dateStyle: d.date, timeZone: "UTC" });
            expect(ja.formatRange(d0, d1)).toBe(d.ja);
        });
    });

    test("dates in reverse order", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "full", timeZone: "UTC" });
        expect(en.formatRange(d1, d0)).toBe("Tuesday, December 7, 2021 – Monday, January 23, 1989");

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "full", timeZone: "UTC" });
        expect(ja.formatRange(d1, d0)).toBe("2021/12/07(火曜日)～1989/01/23(月曜日)");
    });
});

describe("timeStyle", () => {
    // prettier-ignore
    const data = [
        { time: "full", en: "1/23/1989, 7:08:09 AM Coordinated Universal Time – 12/7/2021, 5:40:50 PM Coordinated Universal Time", ja: "1989/1/23 7時08分09秒 協定世界時～2021/12/7 17時40分50秒 協定世界時" },
        { time: "long", en: "1/23/1989, 7:08:09 AM UTC – 12/7/2021, 5:40:50 PM UTC", ja: "1989/1/23 7:08:09 UTC～2021/12/7 17:40:50 UTC" },
        { time: "medium", en: "1/23/1989, 7:08:09 AM – 12/7/2021, 5:40:50 PM", ja: "1989/1/23 7:08:09～2021/12/7 17:40:50" },
        { time: "short", en: "1/23/1989, 7:08 AM – 12/7/2021, 5:40 PM", ja: "1989/1/23 7:08～2021/12/7 17:40" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { timeStyle: d.time, timeZone: "UTC" });
            expect(en.formatRange(d0, d1)).toBe(d.en);

            const ja = new Intl.DateTimeFormat("ja", { timeStyle: d.time, timeZone: "UTC" });
            expect(ja.formatRange(d0, d1)).toBe(d.ja);
        });
    });
});

describe("dateStyle + timeStyle", () => {
    // prettier-ignore
    const data = [
        { date: "full", time: "full", en: "Monday, January 23, 1989 at 7:08:09 AM Coordinated Universal Time – Tuesday, December 7, 2021 at 5:40:50 PM Coordinated Universal Time", ja: "1989/1/23月曜日 7時08分09秒 協定世界時～2021/12/7火曜日 17時40分50秒 協定世界時" },
        { date: "full", time: "long", en: "Monday, January 23, 1989 at 7:08:09 AM UTC – Tuesday, December 7, 2021 at 5:40:50 PM UTC", ja: "1989/1/23月曜日 7:08:09 UTC～2021/12/7火曜日 17:40:50 UTC" },
        { date: "full", time: "medium", en: "Monday, January 23, 1989 at 7:08:09 AM – Tuesday, December 7, 2021 at 5:40:50 PM", ja: "1989/1/23月曜日 7:08:09～2021/12/7火曜日 17:40:50" },
        { date: "full", time: "short", en: "Monday, January 23, 1989 at 7:08 AM – Tuesday, December 7, 2021 at 5:40 PM", ja: "1989/1/23月曜日 7:08～2021/12/7火曜日 17:40" },
        { date: "long", time: "full", en: "January 23, 1989 at 7:08:09 AM Coordinated Universal Time – December 7, 2021 at 5:40:50 PM Coordinated Universal Time", ja: "1989/1/23 7時08分09秒 協定世界時～2021/12/7 17時40分50秒 協定世界時" },
        { date: "long", time: "long", en: "January 23, 1989 at 7:08:09 AM UTC – December 7, 2021 at 5:40:50 PM UTC", ja: "1989/1/23 7:08:09 UTC～2021/12/7 17:40:50 UTC" },
        { date: "long", time: "medium", en: "January 23, 1989 at 7:08:09 AM – December 7, 2021 at 5:40:50 PM", ja: "1989/1/23 7:08:09～2021/12/7 17:40:50" },
        { date: "long", time: "short", en: "January 23, 1989 at 7:08 AM – December 7, 2021 at 5:40 PM", ja: "1989/1/23 7:08～2021/12/7 17:40" },
        { date: "medium", time: "full", en: "Jan 23, 1989, 7:08:09 AM Coordinated Universal Time – Dec 7, 2021, 5:40:50 PM Coordinated Universal Time", ja: "1989/01/23 7時08分09秒 協定世界時～2021/12/07 17時40分50秒 協定世界時" },
        { date: "medium", time: "long", en: "Jan 23, 1989, 7:08:09 AM UTC – Dec 7, 2021, 5:40:50 PM UTC", ja: "1989/01/23 7:08:09 UTC～2021/12/07 17:40:50 UTC" },
        { date: "medium", time: "medium", en: "Jan 23, 1989, 7:08:09 AM – Dec 7, 2021, 5:40:50 PM", ja: "1989/01/23 7:08:09～2021/12/07 17:40:50" },
        { date: "medium", time: "short", en: "Jan 23, 1989, 7:08 AM – Dec 7, 2021, 5:40 PM", ja: "1989/01/23 7:08～2021/12/07 17:40" },
        { date: "short", time: "full", en: "1/23/89, 7:08:09 AM Coordinated Universal Time – 12/7/21, 5:40:50 PM Coordinated Universal Time", ja: "1989/01/23 7時08分09秒 協定世界時～2021/12/07 17時40分50秒 協定世界時" },
        { date: "short", time: "long", en: "1/23/89, 7:08:09 AM UTC – 12/7/21, 5:40:50 PM UTC", ja: "1989/01/23 7:08:09 UTC～2021/12/07 17:40:50 UTC" },
        { date: "short", time: "medium", en: "1/23/89, 7:08:09 AM – 12/7/21, 5:40:50 PM", ja: "1989/01/23 7:08:09～2021/12/07 17:40:50" },
        { date: "short", time: "short", en: "1/23/89, 7:08 AM – 12/7/21, 5:40 PM", ja: "1989/01/23 7:08～2021/12/07 17:40" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                dateStyle: d.date,
                timeStyle: d.time,
                timeZone: "UTC",
            });
            expect(en.formatRange(d0, d1)).toBe(d.en);

            const ja = new Intl.DateTimeFormat("ja", {
                dateStyle: d.date,
                timeStyle: d.time,
                timeZone: "UTC",
            });
            expect(ja.formatRange(d0, d1)).toBe(d.ja);
        });
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
        expect(formatter.formatRange(plainDate1, plainDate2)).toBe("1989-01-23 – 2024-11-27");
    });

    test("Temporal.PlainYearMonth", () => {
        const plainYearMonth1 = new Temporal.PlainYearMonth(1989, 1);
        const plainYearMonth2 = new Temporal.PlainYearMonth(2024, 11);
        expect(formatter.formatRange(plainYearMonth1, plainYearMonth2)).toBe("1989-01 – 2024-11");
    });

    test("Temporal.PlainMonthDay", () => {
        const plainMonthDay1 = new Temporal.PlainMonthDay(1, 23);
        const plainMonthDay2 = new Temporal.PlainMonthDay(11, 27);
        expect(formatter.formatRange(plainMonthDay1, plainMonthDay2)).toBe("01-23 – 11-27");
    });

    test("Temporal.PlainTime", () => {
        const plainTime1 = new Temporal.PlainTime(8, 10, 51);
        const plainTime2 = new Temporal.PlainTime(20, 41, 9);
        expect(formatter.formatRange(plainTime1, plainTime2)).toBe("8:10:51 AM – 8:41:09 PM");
    });

    test("Temporal.Instant", () => {
        const instant1 = new Temporal.Instant(601546251000000000n);
        const instant2 = new Temporal.Instant(1732740069000000000n);
        expect(formatter.formatRange(instant1, instant2)).toBe(
            "1989-01-23, 8:10:51 AM – 2024-11-27, 8:41:09 PM"
        );
    });
});
