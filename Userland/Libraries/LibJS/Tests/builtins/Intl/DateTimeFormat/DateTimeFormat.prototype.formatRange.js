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
        expect(en.formatRange(d0, d0)).toBe("7:08:09\u202fAM");
        expect(en.formatRange(d1, d1)).toBe("5:40:50\u202fPM");

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
        expect(en.formatRange(d0, d0)).toBe("January 23, 1989 at 7:08:09\u202fAM");
        expect(en.formatRange(d1, d1)).toBe("December 07, 2021 at 5:40:50\u202fPM");

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
        expect(en.formatRange(d0, d0)).toBe("Monday, January 23, 1989 at 7:08:09\u202fAM");
        expect(en.formatRange(d1, d1)).toBe("Tuesday, December 7, 2021 at 5:40:50\u202fPM");

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
        { date: "full", en: "Monday, January 23, 1989\u2009–\u2009Tuesday, December 7, 2021", ja: "1989/01/23(月曜日)～2021/12/07(火曜日)" },
        { date: "long", en: "January 23, 1989\u2009–\u2009December 7, 2021", ja: "1989/01/23～2021/12/07" },
        { date: "medium", en: "Jan 23, 1989\u2009–\u2009Dec 7, 2021", ja: "1989/01/23～2021/12/07" },
        { date: "short", en: "1/23/89\u2009–\u200912/7/21", ja: "1989/01/23～2021/12/07" },
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
        expect(en.formatRange(d1, d0)).toBe(
            "Tuesday, December 7, 2021\u2009–\u2009Monday, January 23, 1989"
        );

        const ja = new Intl.DateTimeFormat("ja", { dateStyle: "full", timeZone: "UTC" });
        expect(ja.formatRange(d1, d0)).toBe("2021/12/07(火曜日)～1989/01/23(月曜日)");
    });
});

describe("timeStyle", () => {
    // prettier-ignore
    const data = [
        { time: "full", en: "1/23/1989, 7:08:09\u202fAM Coordinated Universal Time\u2009–\u200912/7/2021, 5:40:50\u202fPM Coordinated Universal Time", ja: "1989/1/23 7時08分09秒 協定世界時～2021/12/7 17時40分50秒 協定世界時" },
        { time: "long", en: "1/23/1989, 7:08:09\u202fAM UTC\u2009–\u200912/7/2021, 5:40:50\u202fPM UTC", ja: "1989/1/23 7:08:09 UTC～2021/12/7 17:40:50 UTC" },
        { time: "medium", en: "1/23/1989, 7:08:09\u202fAM\u2009–\u200912/7/2021, 5:40:50\u202fPM", ja: "1989/1/23 7:08:09～2021/12/7 17:40:50" },
        { time: "short", en: "1/23/1989, 7:08\u202fAM\u2009–\u200912/7/2021, 5:40\u202fPM", ja: "1989/1/23 7:08～2021/12/7 17:40" },
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
        { date: "full", time: "full", en: "Monday, January 23, 1989 at 7:08:09\u202fAM Coordinated Universal Time\u2009–\u2009Tuesday, December 7, 2021 at 5:40:50\u202fPM Coordinated Universal Time", ja: "1989/1/23月曜日 7時08分09秒 協定世界時～2021/12/7火曜日 17時40分50秒 協定世界時" },
        { date: "full", time: "long", en: "Monday, January 23, 1989 at 7:08:09\u202fAM UTC\u2009–\u2009Tuesday, December 7, 2021 at 5:40:50\u202fPM UTC", ja: "1989/1/23月曜日 7:08:09 UTC～2021/12/7火曜日 17:40:50 UTC" },
        { date: "full", time: "medium", en: "Monday, January 23, 1989 at 7:08:09\u202fAM\u2009–\u2009Tuesday, December 7, 2021 at 5:40:50\u202fPM", ja: "1989/1/23月曜日 7:08:09～2021/12/7火曜日 17:40:50" },
        { date: "full", time: "short", en: "Monday, January 23, 1989 at 7:08\u202fAM\u2009–\u2009Tuesday, December 7, 2021 at 5:40\u202fPM", ja: "1989/1/23月曜日 7:08～2021/12/7火曜日 17:40" },
        { date: "long", time: "full", en: "January 23, 1989 at 7:08:09\u202fAM Coordinated Universal Time\u2009–\u2009December 7, 2021 at 5:40:50\u202fPM Coordinated Universal Time", ja: "1989/1/23 7時08分09秒 協定世界時～2021/12/7 17時40分50秒 協定世界時" },
        { date: "long", time: "long", en: "January 23, 1989 at 7:08:09\u202fAM UTC\u2009–\u2009December 7, 2021 at 5:40:50\u202fPM UTC", ja: "1989/1/23 7:08:09 UTC～2021/12/7 17:40:50 UTC" },
        { date: "long", time: "medium", en: "January 23, 1989 at 7:08:09\u202fAM\u2009–\u2009December 7, 2021 at 5:40:50\u202fPM", ja: "1989/1/23 7:08:09～2021/12/7 17:40:50" },
        { date: "long", time: "short", en: "January 23, 1989 at 7:08\u202fAM\u2009–\u2009December 7, 2021 at 5:40\u202fPM", ja: "1989/1/23 7:08～2021/12/7 17:40" },
        { date: "medium", time: "full", en: "Jan 23, 1989, 7:08:09\u202fAM Coordinated Universal Time\u2009–\u2009Dec 7, 2021, 5:40:50\u202fPM Coordinated Universal Time", ja: "1989/01/23 7時08分09秒 協定世界時～2021/12/07 17時40分50秒 協定世界時" },
        { date: "medium", time: "long", en: "Jan 23, 1989, 7:08:09\u202fAM UTC\u2009–\u2009Dec 7, 2021, 5:40:50\u202fPM UTC", ja: "1989/01/23 7:08:09 UTC～2021/12/07 17:40:50 UTC" },
        { date: "medium", time: "medium", en: "Jan 23, 1989, 7:08:09\u202fAM\u2009–\u2009Dec 7, 2021, 5:40:50\u202fPM", ja: "1989/01/23 7:08:09～2021/12/07 17:40:50" },
        { date: "medium", time: "short", en: "Jan 23, 1989, 7:08\u202fAM\u2009–\u2009Dec 7, 2021, 5:40\u202fPM", ja: "1989/01/23 7:08～2021/12/07 17:40" },
        { date: "short", time: "full", en: "1/23/89, 7:08:09\u202fAM Coordinated Universal Time\u2009–\u200912/7/21, 5:40:50\u202fPM Coordinated Universal Time", ja: "1989/01/23 7時08分09秒 協定世界時～2021/12/07 17時40分50秒 協定世界時" },
        { date: "short", time: "long", en: "1/23/89, 7:08:09\u202fAM UTC\u2009–\u200912/7/21, 5:40:50\u202fPM UTC", ja: "1989/01/23 7:08:09 UTC～2021/12/07 17:40:50 UTC" },
        { date: "short", time: "medium", en: "1/23/89, 7:08:09\u202fAM\u2009–\u200912/7/21, 5:40:50\u202fPM", ja: "1989/01/23 7:08:09～2021/12/07 17:40:50" },
        { date: "short", time: "short", en: "1/23/89, 7:08\u202fAM\u2009–\u200912/7/21, 5:40\u202fPM", ja: "1989/01/23 7:08～2021/12/07 17:40" },
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
