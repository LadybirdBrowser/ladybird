describe("errors", () => {
    test("called on non-DateTimeFormat object", () => {
        expect(() => {
            Intl.DateTimeFormat.prototype.formatToParts(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.DateTimeFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            Intl.DateTimeFormat().formatToParts(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            Intl.DateTimeFormat().formatToParts(1n);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("time value cannot be clipped", () => {
        expect(() => {
            Intl.DateTimeFormat().formatToParts(NaN);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

        expect(() => {
            Intl.DateTimeFormat().formatToParts(-8.65e15);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

        expect(() => {
            Intl.DateTimeFormat().formatToParts(8.65e15);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");
    });

    test("Temporal object must have same calendar", () => {
        const formatter = new Intl.DateTimeFormat([], { calendar: "iso8601" });

        expect(() => {
            formatter.formatToParts(new Temporal.PlainDate(1972, 1, 1, "gregory"));
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDate with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            formatter.formatToParts(new Temporal.PlainYearMonth(1972, 1, "gregory"));
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainYearMonth with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            formatter.formatToParts(new Temporal.PlainMonthDay(1, 1, "gregory"));
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainMonthDay with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            formatter.formatToParts(
                new Temporal.PlainDateTime(1972, 1, 1, 8, 45, 56, 123, 345, 789, "gregory")
            );
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDateTime with calendar 'gregory' in locale with calendar 'iso8601'"
        );
    });

    test("cannot format Temporal.ZonedDateTime", () => {
        expect(() => {
            new Intl.DateTimeFormat().formatToParts(new Temporal.ZonedDateTime(0n, "UTC"));
        }).toThrowWithMessage(
            TypeError,
            "Cannot format Temporal.ZonedDateTime, use Temporal.ZonedDateTime.prototype.toLocaleString"
        );
    });

    test("Temporal fields must overlap formatter", () => {
        const yearFormatter = new Intl.DateTimeFormat([], { year: "numeric", calendar: "iso8601" });
        const plainMonthDay = new Temporal.PlainMonthDay(1, 1);

        const dayFormatter = new Intl.DateTimeFormat([], { day: "numeric", calendar: "iso8601" });
        const plainYearMonth = new Temporal.PlainYearMonth(1972, 1);

        expect(() => {
            yearFormatter.formatToParts(plainMonthDay);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainMonthDay");

        expect(() => {
            dayFormatter.formatToParts(plainYearMonth);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainYearMonth");
    });
});

const d = Date.UTC(1989, 0, 23, 7, 8, 9, 45);

describe("dateStyle", () => {
    test("full", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "full", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "weekday", value: "Monday" },
            { type: "literal", value: ", " },
            { type: "month", value: "January" },
            { type: "literal", value: " " },
            { type: "day", value: "23" },
            { type: "literal", value: ", " },
            { type: "year", value: "1989" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { dateStyle: "full", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "weekday", value: "الاثنين" },
            { type: "literal", value: "، " },
            { type: "day", value: "٢٣" },
            { type: "literal", value: " " },
            { type: "month", value: "يناير" },
            { type: "literal", value: " " },
            { type: "year", value: "١٩٨٩" },
        ]);
    });

    test("long", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "long", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "month", value: "January" },
            { type: "literal", value: " " },
            { type: "day", value: "23" },
            { type: "literal", value: ", " },
            { type: "year", value: "1989" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { dateStyle: "long", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "day", value: "٢٣" },
            { type: "literal", value: " " },
            { type: "month", value: "يناير" },
            { type: "literal", value: " " },
            { type: "year", value: "١٩٨٩" },
        ]);
    });

    test("medium", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "medium", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "month", value: "Jan" },
            { type: "literal", value: " " },
            { type: "day", value: "23" },
            { type: "literal", value: ", " },
            { type: "year", value: "1989" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
            dateStyle: "medium",
            timeZone: "UTC",
        });
        expect(ar.formatToParts(d)).toEqual([
            { type: "day", value: "٢٣" },
            { type: "literal", value: "‏/" },
            { type: "month", value: "٠١" },
            { type: "literal", value: "‏/" },
            { type: "year", value: "١٩٨٩" },
        ]);
    });

    test("short", () => {
        const en = new Intl.DateTimeFormat("en", { dateStyle: "short", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "month", value: "1" },
            { type: "literal", value: "/" },
            { type: "day", value: "23" },
            { type: "literal", value: "/" },
            { type: "year", value: "89" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { dateStyle: "short", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "day", value: "٢٣" },
            { type: "literal", value: "‏/" },
            { type: "month", value: "١" },
            { type: "literal", value: "‏/" },
            { type: "year", value: "١٩٨٩" },
        ]);
    });
});

describe("timeStyle", () => {
    test("full", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "full", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "Coordinated Universal Time" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { timeStyle: "full", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "التوقيت العالمي المنسق" },
        ]);
    });

    test("long", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "long", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "UTC" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { timeStyle: "long", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
            { type: "literal", value: " " },
            { type: "timeZoneName", value: "UTC" },
        ]);
    });

    test("medium", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "medium", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
            timeStyle: "medium",
            timeZone: "UTC",
        });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
        ]);
    });

    test("short", () => {
        const en = new Intl.DateTimeFormat("en", { timeStyle: "short", timeZone: "UTC" });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: ":" },
            { type: "minute", value: "08" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { timeStyle: "short", timeZone: "UTC" });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: ":" },
            { type: "minute", value: "٠٨" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "ص" },
        ]);
    });
});

describe("special cases", () => {
    test("dayPeriod", () => {
        const en = new Intl.DateTimeFormat("en", {
            dayPeriod: "long",
            hour: "numeric",
            timeZone: "UTC",
        });
        expect(en.formatToParts(d)).toEqual([
            { type: "hour", value: "7" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "in the morning" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
            dayPeriod: "long",
            hour: "numeric",
            timeZone: "UTC",
        });
        expect(ar.formatToParts(d)).toEqual([
            { type: "hour", value: "٧" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "صباحًا" },
        ]);
    });

    test("fractionalSecondDigits", () => {
        const en = new Intl.DateTimeFormat("en", {
            fractionalSecondDigits: 3,
            second: "numeric",
            minute: "numeric",
            timeZone: "UTC",
        });
        expect(en.formatToParts(d)).toEqual([
            { type: "minute", value: "08" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: "." },
            { type: "fractionalSecond", value: "045" },
        ]);

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
            fractionalSecondDigits: 3,
            second: "numeric",
            minute: "numeric",
            timeZone: "UTC",
        });
        expect(ar.formatToParts(d)).toEqual([
            { type: "minute", value: "٠٨" },
            { type: "literal", value: ":" },
            { type: "second", value: "٠٩" },
            { type: "literal", value: "٫" },
            { type: "fractionalSecond", value: "٠٤٥" },
        ]);
    });
});

describe("Temporal objects", () => {
    const formatter = new Intl.DateTimeFormat("en", {
        calendar: "iso8601",
        timeZone: "UTC",
    });

    test("Temporal.PlainDate", () => {
        const plainDate = new Temporal.PlainDate(1989, 1, 23);
        expect(formatter.formatToParts(plainDate)).toEqual([
            { type: "year", value: "1989" },
            { type: "literal", value: "-" },
            { type: "month", value: "01" },
            { type: "literal", value: "-" },
            { type: "day", value: "23" },
        ]);
    });

    test("Temporal.PlainYearMonth", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(1989, 1);
        expect(formatter.formatToParts(plainYearMonth)).toEqual([
            { type: "year", value: "1989" },
            { type: "literal", value: "-" },
            { type: "month", value: "01" },
        ]);
    });

    test("Temporal.PlainMonthDay", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(1, 23);
        expect(formatter.formatToParts(plainMonthDay)).toEqual([
            { type: "month", value: "01" },
            { type: "literal", value: "-" },
            { type: "day", value: "23" },
        ]);
    });

    test("Temporal.PlainTime", () => {
        const plainTime = new Temporal.PlainTime(8, 10, 51);
        expect(formatter.formatToParts(plainTime)).toEqual([
            { type: "hour", value: "8" },
            { type: "literal", value: ":" },
            { type: "minute", value: "10" },
            { type: "literal", value: ":" },
            { type: "second", value: "51" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "AM" },
        ]);
    });

    test("Temporal.Instant", () => {
        const instant = new Temporal.Instant(1732740069000000000n);
        expect(formatter.formatToParts(instant)).toEqual([
            { type: "year", value: "2024" },
            { type: "literal", value: "-" },
            { type: "month", value: "11" },
            { type: "literal", value: "-" },
            { type: "day", value: "27" },
            { type: "literal", value: ", " },
            { type: "hour", value: "8" },
            { type: "literal", value: ":" },
            { type: "minute", value: "41" },
            { type: "literal", value: ":" },
            { type: "second", value: "09" },
            { type: "literal", value: " " },
            { type: "dayPeriod", value: "PM" },
        ]);
    });
});
