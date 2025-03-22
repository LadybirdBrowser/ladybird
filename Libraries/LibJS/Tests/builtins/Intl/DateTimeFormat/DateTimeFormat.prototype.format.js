describe("errors", () => {
    test("called on non-DateTimeFormat object", () => {
        expect(() => {
            Intl.DateTimeFormat.prototype.format;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.DateTimeFormat");

        expect(() => {
            Intl.DateTimeFormat.prototype.format(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.DateTimeFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            Intl.DateTimeFormat().format(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            Intl.DateTimeFormat().format(1n);
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("time value cannot be clipped", () => {
        expect(() => {
            Intl.DateTimeFormat().format(NaN);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

        expect(() => {
            Intl.DateTimeFormat().format(-8.65e15);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");

        expect(() => {
            Intl.DateTimeFormat().format(8.65e15);
        }).toThrowWithMessage(RangeError, "Time value must be between -8.64E15 and 8.64E15");
    });

    test("Temporal object must have same calendar", () => {
        const formatter = new Intl.DateTimeFormat([], { calendar: "iso8601" });

        expect(() => {
            formatter.format(new Temporal.PlainDate(1972, 1, 1, "gregory"));
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDate with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            formatter.format(new Temporal.PlainYearMonth(1972, 1, "gregory"));
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainYearMonth with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            formatter.format(new Temporal.PlainMonthDay(1, 1, "gregory"));
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainMonthDay with calendar 'gregory' in locale with calendar 'iso8601'"
        );

        expect(() => {
            formatter.format(
                new Temporal.PlainDateTime(1972, 1, 1, 8, 45, 56, 123, 345, 789, "gregory")
            );
        }).toThrowWithMessage(
            RangeError,
            "Cannot format Temporal.PlainDateTime with calendar 'gregory' in locale with calendar 'iso8601'"
        );
    });

    test("cannot format Temporal.ZonedDateTime", () => {
        expect(() => {
            new Intl.DateTimeFormat().format(new Temporal.ZonedDateTime(0n, "UTC"));
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
            yearFormatter.format(plainMonthDay);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainMonthDay");

        expect(() => {
            dayFormatter.format(plainYearMonth);
        }).toThrowWithMessage(TypeError, "Unable to determine format for Temporal.PlainYearMonth");
    });
});

const d0 = Date.UTC(2021, 11, 7, 17, 40, 50, 456);
const d1 = Date.UTC(1989, 0, 23, 7, 8, 9, 45);

describe("dateStyle", () => {
    // prettier-ignore
    const data = [
        { date: "full", en0: "Tuesday, December 7, 2021", en1: "Monday, January 23, 1989", ar0: "الثلاثاء، ٧ ديسمبر ٢٠٢١", ar1: "الاثنين، ٢٣ يناير ١٩٨٩" },
        { date: "long", en0: "December 7, 2021", en1: "January 23, 1989", ar0: "٧ ديسمبر ٢٠٢١", ar1: "٢٣ يناير ١٩٨٩" },
        { date: "medium", en0: "Dec 7, 2021", en1: "Jan 23, 1989", ar0: "٠٧‏/١٢‏/٢٠٢١", ar1: "٢٣‏/٠١‏/١٩٨٩" },
        { date: "short", en0: "12/7/21", en1: "1/23/89", ar0: "٧‏/١٢‏/٢٠٢١", ar1: "٢٣‏/١‏/١٩٨٩" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { dateStyle: d.date, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                dateStyle: d.date,
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("timeStyle", () => {
    // prettier-ignore
    const data = [
        { time: "full", en0: "5:40:50 PM Coordinated Universal Time", en1: "7:08:09 AM Coordinated Universal Time", ar0: "٥:٤٠:٥٠ م التوقيت العالمي المنسق", ar1: "٧:٠٨:٠٩ ص التوقيت العالمي المنسق" },
        { time: "long", en0: "5:40:50 PM UTC", en1: "7:08:09 AM UTC", ar0: "٥:٤٠:٥٠ م UTC", ar1: "٧:٠٨:٠٩ ص UTC" },
        { time: "medium", en0: "5:40:50 PM", en1: "7:08:09 AM", ar0: "٥:٤٠:٥٠ م", ar1: "٧:٠٨:٠٩ ص" },
        { time: "short", en0: "5:40 PM", en1: "7:08 AM", ar0: "٥:٤٠ م", ar1: "٧:٠٨ ص" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { timeStyle: d.time, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                timeStyle: d.time,
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("dateStyle + timeStyle", () => {
    // prettier-ignore
    const data = [
        { date: "full", time: "full", en: "Tuesday, December 7, 2021 at 5:40:50 PM Coordinated Universal Time", ar: "الثلاثاء، ٧ ديسمبر ٢٠٢١ في ٥:٤٠:٥٠ م التوقيت العالمي المنسق" },
        { date: "full", time: "long", en: "Tuesday, December 7, 2021 at 5:40:50 PM UTC", ar: "الثلاثاء، ٧ ديسمبر ٢٠٢١ في ٥:٤٠:٥٠ م UTC" },
        { date: "full", time: "medium", en: "Tuesday, December 7, 2021 at 5:40:50 PM", ar: "الثلاثاء، ٧ ديسمبر ٢٠٢١ في ٥:٤٠:٥٠ م" },
        { date: "full", time: "short", en: "Tuesday, December 7, 2021 at 5:40 PM", ar: "الثلاثاء، ٧ ديسمبر ٢٠٢١ في ٥:٤٠ م" },
        { date: "long", time: "full", en: "December 7, 2021 at 5:40:50 PM Coordinated Universal Time", ar: "٧ ديسمبر ٢٠٢١ في ٥:٤٠:٥٠ م التوقيت العالمي المنسق" },
        { date: "long", time: "long", en: "December 7, 2021 at 5:40:50 PM UTC", ar: "٧ ديسمبر ٢٠٢١ في ٥:٤٠:٥٠ م UTC" },
        { date: "long", time: "medium", en: "December 7, 2021 at 5:40:50 PM", ar: "٧ ديسمبر ٢٠٢١ في ٥:٤٠:٥٠ م" },
        { date: "long", time: "short", en: "December 7, 2021 at 5:40 PM", ar: "٧ ديسمبر ٢٠٢١ في ٥:٤٠ م" },
        { date: "medium", time: "full", en: "Dec 7, 2021, 5:40:50 PM Coordinated Universal Time", ar: "٠٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م التوقيت العالمي المنسق" },
        { date: "medium", time: "long", en: "Dec 7, 2021, 5:40:50 PM UTC", ar: "٠٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م UTC" },
        { date: "medium", time: "medium", en: "Dec 7, 2021, 5:40:50 PM", ar: "٠٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م" },
        { date: "medium", time: "short", en: "Dec 7, 2021, 5:40 PM", ar: "٠٧‏/١٢‏/٢٠٢١، ٥:٤٠ م" },
        { date: "short", time: "full", en: "12/7/21, 5:40:50 PM Coordinated Universal Time", ar: "٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م التوقيت العالمي المنسق" },
        { date: "short", time: "long", en: "12/7/21, 5:40:50 PM UTC", ar: "٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م UTC" },
        { date: "short", time: "medium", en: "12/7/21, 5:40:50 PM", ar: "٧‏/١٢‏/٢٠٢١، ٥:٤٠:٥٠ م" },
        { date: "short", time: "short", en: "12/7/21, 5:40 PM", ar: "٧‏/١٢‏/٢٠٢١، ٥:٤٠ م" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                dateStyle: d.date,
                timeStyle: d.time,
                timeZone: "UTC",
            });
            expect(en.format(d0)).toBe(d.en);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                dateStyle: d.date,
                timeStyle: d.time,
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar);
        });
    });
});

describe("weekday", () => {
    // prettier-ignore
    const data = [
        { weekday: "narrow", en0: "T", en1: "M", ar0: "ث", ar1: "ن" },
        { weekday: "short", en0: "Tue", en1: "Mon", ar0: "الثلاثاء", ar1: "الاثنين" },
        { weekday: "long", en0: "Tuesday", en1: "Monday", ar0: "الثلاثاء", ar1: "الاثنين" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { weekday: d.weekday, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                weekday: d.weekday,
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("era", () => {
    // prettier-ignore
    const data = [
        { era: "narrow", en0: "12/7/2021 A", en1: "1/23/1989 A", ar0: "٠٧-١٢-٢٠٢١ م", ar1: "٢٣-٠١-١٩٨٩ م" },
        { era: "short", en0: "12/7/2021 AD", en1: "1/23/1989 AD", ar0: "٠٧-١٢-٢٠٢١ م", ar1: "٢٣-٠١-١٩٨٩ م" },
        { era: "long", en0: "12/7/2021 Anno Domini", en1: "1/23/1989 Anno Domini", ar0: "٠٧-١٢-٢٠٢١ ميلادي", ar1: "٢٣-٠١-١٩٨٩ ميلادي" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { era: d.era, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { era: d.era, timeZone: "UTC" });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });

    test("Year 1 BC (ISO year 0)", () => {
        let year1BC = new Date(Date.UTC(0));
        year1BC.setUTCFullYear(0);

        const en = new Intl.DateTimeFormat("en", { era: "short", timeZone: "UTC" });
        expect(en.format(year1BC)).toBe("1/1/1 BC");

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { era: "short", timeZone: "UTC" });
        expect(ar.format(year1BC)).toBe("٠١-٠١-١ ق.م");
    });
});

describe("year", () => {
    // prettier-ignore
    const data = [
        { year: "2-digit", en0: "21", en1: "89", ar0: "٢١", ar1: "٨٩" },
        { year: "numeric", en0: "2021", en1: "1989", ar0: "٢٠٢١", ar1: "١٩٨٩" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { year: d.year, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { year: d.year, timeZone: "UTC" });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("month", () => {
    // prettier-ignore
    const data = [
        { month: "2-digit", en0: "12", en1: "01", ar0: "١٢", ar1: "٠١" },
        { month: "numeric", en0: "12", en1: "1", ar0: "١٢", ar1: "١" },
        { month: "narrow", en0: "D", en1: "J", ar0: "د", ar1: "ي" },
        { month: "short", en0: "Dec", en1: "Jan", ar0: "ديسمبر", ar1: "يناير" },
        { month: "long", en0: "December", en1: "January", ar0: "ديسمبر", ar1: "يناير" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { month: d.month, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { month: d.month, timeZone: "UTC" });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("day", () => {
    // prettier-ignore
    const data = [
        { day: "2-digit", en0: "07", en1: "23", ar0: "٠٧", ar1: "٢٣" },
        { day: "numeric", en0: "7", en1: "23", ar0: "٧", ar1: "٢٣" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { day: d.day, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { day: d.day, timeZone: "UTC" });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("dayPeriod", () => {
    // prettier-ignore
    const data = [
        { dayPeriod: "narrow", en0: "5 in the afternoon", en1: "7 in the morning", ar0: "٥ بعد الظهر", ar1: "٧ صباحًا", as0: "pm ৫", as1: "am ৭"},
        { dayPeriod: "short", en0: "5 in the afternoon", en1: "7 in the morning", ar0: "٥ بعد الظهر", ar1: "٧ ص", as0: "PM ৫", as1: "AM ৭"},
        { dayPeriod: "long", en0: "5 in the afternoon", en1: "7 in the morning", ar0: "٥ بعد الظهر", ar1: "٧ صباحًا", as0: "PM ৫", as1: "AM ৭"},
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                dayPeriod: d.dayPeriod,
                hour: "numeric",
                timeZone: "UTC",
            });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                dayPeriod: d.dayPeriod,
                hour: "numeric",
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);

            const as = new Intl.DateTimeFormat("as", {
                dayPeriod: d.dayPeriod,
                hour: "numeric",
                timeZone: "UTC",
            });
            expect(as.format(d0)).toBe(d.as0);
            expect(as.format(d1)).toBe(d.as1);
        });
    });

    test("flexible day period rolls over midnight", () => {
        const en = new Intl.DateTimeFormat("en", {
            hour: "numeric",
            dayPeriod: "short",
            timeZone: "UTC",
        });

        // For the en locale, these times (05:00 and 23:00) fall in the flexible day period range of
        // [21:00, 06:00), on either side of midnight.
        const date1 = Date.UTC(2017, 11, 12, 5, 0, 0, 0);
        const date2 = Date.UTC(2017, 11, 12, 23, 0, 0, 0);

        expect(en.format(date1)).toBe("5 at night");
        expect(en.format(date2)).toBe("11 at night");
    });

    test("noon", () => {
        const date0 = Date.UTC(2017, 11, 12, 12, 0, 0, 0);
        const date1 = Date.UTC(2017, 11, 12, 12, 1, 0, 0);
        const date2 = Date.UTC(2017, 11, 12, 12, 0, 1, 0);
        const date3 = Date.UTC(2017, 11, 12, 12, 0, 0, 500);

        // prettier-ignore
        const data = [
            { minute: undefined, second: undefined, fractionalSecondDigits: undefined, en0: "12 noon", en1: "12 noon", en2: "12 noon", en3: "12 noon", ar0: "١٢ ظهرًا", ar1: "١٢ ظهرًا", ar2: "١٢ ظهرًا", ar3: "١٢ ظهرًا" },
            { minute: "numeric", second: undefined, fractionalSecondDigits: undefined, en0: "12:00 noon", en1: "12:01 in the afternoon", en2: "12:00 noon", en3: "12:00 noon", ar0: "١٢:٠٠ ظهرًا", ar1: "١٢:٠١ ظهرًا", ar2: "١٢:٠٠ ظهرًا", ar3: "١٢:٠٠ ظهرًا" },
            { minute: "numeric", second: "numeric", fractionalSecondDigits: undefined, en0: "12:00:00 noon", en1: "12:01:00 in the afternoon", en2: "12:00:01 in the afternoon", en3: "12:00:00 noon", ar0: "١٢:٠٠:٠٠ ظهرًا", ar1: "١٢:٠١:٠٠ ظهرًا", ar2: "١٢:٠٠:٠١ ظهرًا", ar3: "١٢:٠٠:٠٠ ظهرًا" },
            { minute: "numeric", second: "numeric", fractionalSecondDigits: 1, en0: "12:00:00.0 noon", en1: "12:01:00.0 in the afternoon", en2: "12:00:01.0 in the afternoon", en3: "12:00:00.5 noon", ar0: "١٢:٠٠:٠٠٫٠ ظهرًا", ar1: "١٢:٠١:٠٠٫٠ ظهرًا", ar2: "١٢:٠٠:٠١٫٠ ظهرًا", ar3: "١٢:٠٠:٠٠٫٥ ظهرًا" },
        ];

        // The en locale includes the "noon" fixed day period, whereas the ar locale does not.
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                hour: "numeric",
                dayPeriod: "short",
                timeZone: "UTC",
                minute: d.minute,
                second: d.second,
                fractionalSecondDigits: d.fractionalSecondDigits,
            });
            expect(en.format(date0)).toBe(d.en0);
            expect(en.format(date1)).toBe(d.en1);
            expect(en.format(date2)).toBe(d.en2);
            expect(en.format(date3)).toBe(d.en3);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                hour: "numeric",
                dayPeriod: "short",
                timeZone: "UTC",
                minute: d.minute,
                second: d.second,
                fractionalSecondDigits: d.fractionalSecondDigits,
            });
            expect(ar.format(date0)).toBe(d.ar0);
            expect(ar.format(date1)).toBe(d.ar1);
            expect(ar.format(date2)).toBe(d.ar2);
            expect(ar.format(date3)).toBe(d.ar3);
        });
    });

    test("dayPeriod without time", () => {
        // prettier-ignore
        const data = [
            { dayPeriod: "narrow", en0: "in the afternoon", en1: "in the morning", ar0: "بعد الظهر", ar1: "صباحًا", as0: "pm", as1: "am"},
            { dayPeriod: "short", en0: "in the afternoon", en1: "in the morning", ar0: "بعد الظهر", ar1: "ص", as0: "PM", as1: "AM"},
            { dayPeriod: "long", en0: "in the afternoon", en1: "in the morning", ar0: "بعد الظهر", ar1: "صباحًا", as0: "PM", as1: "AM"},
        ];

        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                dayPeriod: d.dayPeriod,
                timeZone: "UTC",
            });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                dayPeriod: d.dayPeriod,
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);

            const as = new Intl.DateTimeFormat("as", {
                dayPeriod: d.dayPeriod,
                timeZone: "UTC",
            });
            expect(as.format(d0)).toBe(d.as0);
            expect(as.format(d1)).toBe(d.as1);
        });
    });
});

describe("hour", () => {
    // prettier-ignore
    const data = [
        { hour: "2-digit", en0: "05 PM", en1: "07 AM", ar0: "٠٥ م", ar1: "٠٧ ص" },
        { hour: "numeric", en0: "5 PM", en1: "7 AM", ar0: "٥ م", ar1: "٧ ص" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", { hour: d.hour, timeZone: "UTC" });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", { hour: d.hour, timeZone: "UTC" });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("minute", () => {
    // prettier-ignore
    const data = [
        { minute: "2-digit", en0: "5:40 PM", en1: "7:08 AM", ar0: "٥:٤٠ م", ar1: "٧:٠٨ ص" },
        { minute: "numeric", en0: "5:40 PM", en1: "7:08 AM", ar0: "٥:٤٠ م", ar1: "٧:٠٨ ص" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                minute: d.minute,
                hour: "numeric",
                timeZone: "UTC",
            });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                minute: d.minute,
                hour: "numeric",
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("second", () => {
    // prettier-ignore
    const data = [
        { second: "2-digit", en0: "40:50", en1: "08:09", ar0: "٤٠:٥٠", ar1: "٠٨:٠٩" },
        { second: "numeric", en0: "40:50", en1: "08:09", ar0: "٤٠:٥٠", ar1: "٠٨:٠٩" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                second: d.second,
                minute: "numeric",
                timeZone: "UTC",
            });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                second: d.second,
                minute: "numeric",
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("fractionalSecondDigits", () => {
    // prettier-ignore
    const data = [
        { fractionalSecondDigits: 1, en0: "40:50.4", en1: "08:09.0", ar0: "٤٠:٥٠٫٤", ar1: "٠٨:٠٩٫٠" },
        { fractionalSecondDigits: 2, en0: "40:50.45", en1: "08:09.04", ar0: "٤٠:٥٠٫٤٥", ar1: "٠٨:٠٩٫٠٤" },
        { fractionalSecondDigits: 3, en0: "40:50.456", en1: "08:09.045", ar0: "٤٠:٥٠٫٤٥٦", ar1: "٠٨:٠٩٫٠٤٥" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                fractionalSecondDigits: d.fractionalSecondDigits,
                second: "numeric",
                minute: "numeric",
                timeZone: "UTC",
            });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                fractionalSecondDigits: d.fractionalSecondDigits,
                second: "numeric",
                minute: "numeric",
                timeZone: "UTC",
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("timeZoneName", () => {
    // prettier-ignore
    const data = [
        { timeZone: "UTC", timeZoneName: "short", en0: "12/7/2021, UTC", en1: "1/23/1989, UTC", ar0: "٧‏/١٢‏/٢٠٢١، UTC", ar1: "٢٣‏/١‏/١٩٨٩، UTC" },
        { timeZone: "UTC", timeZoneName: "long", en0: "12/7/2021, Coordinated Universal Time", en1: "1/23/1989, Coordinated Universal Time", ar0: "٧‏/١٢‏/٢٠٢١، التوقيت العالمي المنسق", ar1: "٢٣‏/١‏/١٩٨٩، التوقيت العالمي المنسق" },
        { timeZone: "UTC", timeZoneName: "shortOffset", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },
        { timeZone: "UTC", timeZoneName: "longOffset", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },
        { timeZone: "UTC", timeZoneName: "shortGeneric", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },
        { timeZone: "UTC", timeZoneName: "longGeneric", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },

        { timeZone: "America/New_York", timeZoneName: "short", en0: "12/7/2021, EST", en1: "1/23/1989, EST", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش-٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش-٥" },
        { timeZone: "America/New_York", timeZoneName: "long", en0: "12/7/2021, Eastern Standard Time", en1: "1/23/1989, Eastern Standard Time", ar0: "٧‏/١٢‏/٢٠٢١، التوقيت الرسمي الشرقي لأمريكا الشمالية", ar1: "٢٣‏/١‏/١٩٨٩، التوقيت الرسمي الشرقي لأمريكا الشمالية" },
        { timeZone: "America/New_York", timeZoneName: "shortOffset", en0: "12/7/2021, GMT-5", en1: "1/23/1989, GMT-5", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش-٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش-٥" },
        { timeZone: "America/New_York", timeZoneName: "longOffset", en0: "12/7/2021, GMT-05:00", en1: "1/23/1989, GMT-05:00", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش-٠٥:٠٠", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش-٠٥:٠٠" },
        { timeZone: "America/New_York", timeZoneName: "shortGeneric", en0: "12/7/2021, ET", en1: "1/23/1989, ET", ar0: "٧‏/١٢‏/٢٠٢١، توقيت نيويورك", ar1: "٢٣‏/١‏/١٩٨٩، توقيت نيويورك" },
        { timeZone: "America/New_York", timeZoneName: "longGeneric", en0: "12/7/2021, Eastern Time", en1: "1/23/1989, Eastern Time", ar0: "٧‏/١٢‏/٢٠٢١، التوقيت الشرقي لأمريكا الشمالية", ar1: "٢٣‏/١‏/١٩٨٩، التوقيت الشرقي لأمريكا الشمالية" },

        { timeZone: "Europe/London", timeZoneName: "short", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },
        { timeZone: "Europe/London", timeZoneName: "long", en0: "12/7/2021, Greenwich Mean Time", en1: "1/23/1989, Greenwich Mean Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، توقيت غرينتش" },
        { timeZone: "Europe/London", timeZoneName: "shortOffset", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },
        { timeZone: "Europe/London", timeZoneName: "longOffset", en0: "12/7/2021, GMT", en1: "1/23/1989, GMT", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش" },
        { timeZone: "Europe/London", timeZoneName: "shortGeneric", en0: "12/7/2021, United Kingdom Time", en1: "1/23/1989, United Kingdom Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت المملكة المتحدة", ar1: "٢٣‏/١‏/١٩٨٩، توقيت المملكة المتحدة" },
        { timeZone: "Europe/London", timeZoneName: "longGeneric", en0: "12/7/2021, United Kingdom Time", en1: "1/23/1989, United Kingdom Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت المملكة المتحدة", ar1: "٢٣‏/١‏/١٩٨٩، توقيت المملكة المتحدة" },

        { timeZone: "America/Los_Angeles", timeZoneName: "short", en0: "12/7/2021, PST", en1: "1/22/1989, PST", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش-٨", ar1: "٢٢‏/١‏/١٩٨٩، غرينتش-٨" },
        { timeZone: "America/Los_Angeles", timeZoneName: "long", en0: "12/7/2021, Pacific Standard Time", en1: "1/22/1989, Pacific Standard Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت المحيط الهادي الرسمي", ar1: "٢٢‏/١‏/١٩٨٩، توقيت المحيط الهادي الرسمي" },
        { timeZone: "America/Los_Angeles", timeZoneName: "shortOffset", en0: "12/7/2021, GMT-8", en1: "1/22/1989, GMT-8", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش-٨", ar1: "٢٢‏/١‏/١٩٨٩، غرينتش-٨" },
        { timeZone: "America/Los_Angeles", timeZoneName: "longOffset", en0: "12/7/2021, GMT-08:00", en1: "1/22/1989, GMT-08:00", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش-٠٨:٠٠", ar1: "٢٢‏/١‏/١٩٨٩، غرينتش-٠٨:٠٠" },
        { timeZone: "America/Los_Angeles", timeZoneName: "shortGeneric", en0: "12/7/2021, PT", en1: "1/22/1989, PT", ar0: "٧‏/١٢‏/٢٠٢١، توقيت لوس انجلوس", ar1: "٢٢‏/١‏/١٩٨٩، توقيت لوس انجلوس" },
        { timeZone: "America/Los_Angeles", timeZoneName: "longGeneric", en0: "12/7/2021, Pacific Time", en1: "1/22/1989, Pacific Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت المحيط الهادي", ar1: "٢٢‏/١‏/١٩٨٩، توقيت المحيط الهادي" },

        { timeZone: "Asia/Kathmandu", timeZoneName: "short", en0: "12/7/2021, GMT+5:45", en1: "1/23/1989, GMT+5:45", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٥:٤٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٥:٤٥" },
        { timeZone: "Asia/Kathmandu", timeZoneName: "long", en0: "12/7/2021, Nepal Time", en1: "1/23/1989, Nepal Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت نيبال", ar1: "٢٣‏/١‏/١٩٨٩، توقيت نيبال" },
        { timeZone: "Asia/Kathmandu", timeZoneName: "shortOffset", en0: "12/7/2021, GMT+5:45", en1: "1/23/1989, GMT+5:45", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٥:٤٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٥:٤٥" },
        { timeZone: "Asia/Kathmandu", timeZoneName: "longOffset", en0: "12/7/2021, GMT+05:45", en1: "1/23/1989, GMT+05:45", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٠٥:٤٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٠٥:٤٥" },
        { timeZone: "Asia/Kathmandu", timeZoneName: "shortGeneric", en0: "12/7/2021, Nepal Time", en1: "1/23/1989, Nepal Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت نيبال", ar1: "٢٣‏/١‏/١٩٨٩، توقيت نيبال" },
        { timeZone: "Asia/Kathmandu", timeZoneName: "longGeneric", en0: "12/7/2021, Nepal Time", en1: "1/23/1989, Nepal Time", ar0: "٧‏/١٢‏/٢٠٢١، توقيت نيبال", ar1: "٢٣‏/١‏/١٩٨٩، توقيت نيبال" },

        { timeZone: "+04:15", timeZoneName: "short", en0: "12/7/2021, GMT+4:15", en1: "1/23/1989, GMT+4:15", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٤:١٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٤:١٥" },
        { timeZone: "+04:15", timeZoneName: "long", en0: "12/7/2021, GMT+04:15", en1: "1/23/1989, GMT+04:15", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٠٤:١٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٠٤:١٥" },
        { timeZone: "+04:15", timeZoneName: "shortOffset", en0: "12/7/2021, GMT+4:15", en1: "1/23/1989, GMT+4:15", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٤:١٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٤:١٥" },
        { timeZone: "+04:15", timeZoneName: "longOffset", en0: "12/7/2021, GMT+04:15", en1: "1/23/1989, GMT+04:15", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٠٤:١٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٠٤:١٥" },
        { timeZone: "+04:15", timeZoneName: "shortGeneric", en0: "12/7/2021, GMT+4:15", en1: "1/23/1989, GMT+4:15", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٤:١٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٤:١٥" },
        { timeZone: "+04:15", timeZoneName: "longGeneric", en0: "12/7/2021, GMT+04:15", en1: "1/23/1989, GMT+04:15", ar0: "٧‏/١٢‏/٢٠٢١، غرينتش+٠٤:١٥", ar1: "٢٣‏/١‏/١٩٨٩، غرينتش+٠٤:١٥" },
    ];

    test("all", () => {
        data.forEach(d => {
            const en = new Intl.DateTimeFormat("en", {
                timeZone: d.timeZone,
                timeZoneName: d.timeZoneName,
            });
            expect(en.format(d0)).toBe(d.en0);
            expect(en.format(d1)).toBe(d.en1);

            const ar = new Intl.DateTimeFormat("ar-u-nu-arab", {
                timeZone: d.timeZone,
                timeZoneName: d.timeZoneName,
            });
            expect(ar.format(d0)).toBe(d.ar0);
            expect(ar.format(d1)).toBe(d.ar1);
        });
    });
});

describe("non-Gregorian calendars", () => {
    test("Hebrew", () => {
        const en = new Intl.DateTimeFormat("en-u-ca-hebrew", {
            dateStyle: "long",
            timeStyle: "long",
            timeZone: "UTC",
        });
        expect(en.format(d0)).toBe("3 Tevet 5782 at 5:40:50 PM UTC");
        expect(en.format(d1)).toBe("17 Shevat 5749 at 7:08:09 AM UTC");

        const ar = new Intl.DateTimeFormat("ar-u-nu-arab-ca-hebrew", {
            dateStyle: "long",
            timeStyle: "long",
            timeZone: "UTC",
        });
        expect(ar.format(d0)).toBe("٣ طيفت ٥٧٨٢ ص في ٥:٤٠:٥٠ م UTC");
        expect(ar.format(d1)).toBe("١٧ شباط ٥٧٤٩ ص في ٧:٠٨:٠٩ ص UTC");
    });

    test("Chinese", () => {
        const en = new Intl.DateTimeFormat("en-u-ca-chinese", {
            dateStyle: "long",
            timeStyle: "long",
            timeZone: "UTC",
        });
        expect(en.format(d0)).toBe("Eleventh Month 4, 2021(xin-chou) at 5:40:50 PM UTC");
        expect(en.format(d1)).toBe("Twelfth Month 16, 1988(wu-chen) at 7:08:09 AM UTC");

        const zh = new Intl.DateTimeFormat("zh-u-ca-chinese", {
            dateStyle: "long",
            timeStyle: "long",
            timeZone: "UTC",
        });
        expect(zh.format(d0)).toBe("2021辛丑年十一月初四 UTC 17:40:50");
        expect(zh.format(d1)).toBe("1988戊辰年腊月十六 UTC 07:08:09");
    });
});

describe("Temporal objects", () => {
    const formatter = new Intl.DateTimeFormat("en", {
        calendar: "iso8601",
        timeZone: "UTC",
    });

    test("Temporal.PlainDate", () => {
        const plainDate = new Temporal.PlainDate(1989, 1, 23);
        expect(formatter.format(plainDate)).toBe("1989-01-23");
    });

    test("Temporal.PlainYearMonth", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(1989, 1);
        expect(formatter.format(plainYearMonth)).toBe("1989-01");
    });

    test("Temporal.PlainMonthDay", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(1, 23);
        expect(formatter.format(plainMonthDay)).toBe("01-23");
    });

    test("Temporal.PlainTime", () => {
        const plainTime = new Temporal.PlainTime(8, 10, 51);
        expect(formatter.format(plainTime)).toBe("8:10:51 AM");
    });

    test("Temporal.Instant", () => {
        const instant = new Temporal.Instant(1732740069000000000n);
        expect(formatter.format(instant)).toBe("2024-11-27, 8:41:09 PM");
    });
});
