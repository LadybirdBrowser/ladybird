describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Intl.DurationFormat.prototype.format).toHaveLength(1);
    });

    test("formats duration correctly", () => {
        const duration = {
            years: 1,
            months: 2,
            weeks: 3,
            days: 3,
            hours: 4,
            minutes: 5,
            seconds: 6,
            milliseconds: 7,
            microseconds: 8,
            nanoseconds: 9,
        };
        expect(new Intl.DurationFormat().format(duration)).toBe(
            "1 yr, 2 mths, 3 wks, 3 days, 4 hr, 5 min, 6 sec, 7 ms, 8 μs, 9 ns"
        );
        expect(new Intl.DurationFormat("en").format(duration)).toBe(
            "1 yr, 2 mths, 3 wks, 3 days, 4 hr, 5 min, 6 sec, 7 ms, 8 μs, 9 ns"
        );
        expect(new Intl.DurationFormat("en", { style: "long" }).format(duration)).toBe(
            "1 year, 2 months, 3 weeks, 3 days, 4 hours, 5 minutes, 6 seconds, 7 milliseconds, 8 microseconds, 9 nanoseconds"
        );
        expect(new Intl.DurationFormat("en", { style: "short" }).format(duration)).toBe(
            "1 yr, 2 mths, 3 wks, 3 days, 4 hr, 5 min, 6 sec, 7 ms, 8 μs, 9 ns"
        );
        expect(new Intl.DurationFormat("en", { style: "narrow" }).format(duration)).toBe(
            "1y 2m 3w 3d 4h 5m 6s 7ms 8μs 9ns"
        );
        expect(new Intl.DurationFormat("en", { style: "digital" }).format(duration)).toBe(
            "1 yr, 2 mths, 3 wks, 3 days, 4:05:06.007008009"
        );
        expect(
            new Intl.DurationFormat("en", {
                style: "narrow",
                nanoseconds: "numeric",
                fractionalDigits: 3,
            }).format(duration)
        ).toBe("1y 2m 3w 3d 4h 5m 6s 7ms 8.009μs");

        expect(new Intl.DurationFormat("de", { style: "long" }).format(duration)).toBe(
            "1 Jahr, 2 Monate, 3 Wochen, 3 Tage, 4 Stunden, 5 Minuten, 6 Sekunden, 7 Millisekunden, 8 Mikrosekunden und 9 Nanosekunden"
        );
        expect(new Intl.DurationFormat("de", { style: "short" }).format(duration)).toBe(
            "1 J, 2 Mon., 3 Wo., 3 Tg., 4 Std., 5 Min., 6 Sek., 7 ms, 8 μs und 9 ns"
        );
        expect(new Intl.DurationFormat("de", { style: "narrow" }).format(duration)).toBe(
            "1 J, 2 M, 3 W, 3 T, 4 Std., 5 Min., 6 Sek., 7 ms, 8 μs und 9 ns"
        );
        expect(new Intl.DurationFormat("de", { style: "digital" }).format(duration)).toBe(
            "1 J, 2 Mon., 3 Wo., 3 Tg. und 4:05:06,007008009"
        );
        expect(
            new Intl.DurationFormat("de", {
                style: "narrow",
                nanoseconds: "numeric",
                fractionalDigits: 3,
            }).format(duration)
        ).toBe("1 J, 2 M, 3 W, 3 T, 4 Std., 5 Min., 6 Sek., 7 ms und 8,009 μs");
    });

    test("always show time fields for digital style", () => {
        const duration1 = {
            years: 1,
            months: 2,
            weeks: 3,
            days: 3,
        };
        const duration2 = {
            years: 1,
            months: 2,
            weeks: 3,
            days: 3,
            hours: 4,
            minutes: 5,
            seconds: 6,
        };

        const en = new Intl.DurationFormat("en", { style: "digital" });
        expect(en.format(duration1)).toBe("1 yr, 2 mths, 3 wks, 3 days, 0:00:00");
        expect(en.format(duration2)).toBe("1 yr, 2 mths, 3 wks, 3 days, 4:05:06");

        const de = new Intl.DurationFormat("de", { style: "digital" });
        expect(de.format(duration1)).toBe("1 J, 2 Mon., 3 Wo., 3 Tg. und 0:00:00");
        expect(de.format(duration2)).toBe("1 J, 2 Mon., 3 Wo., 3 Tg. und 4:05:06");
    });

    test("locale with a time separator string other than ':'", () => {
        const duration = {
            hours: 1,
            minutes: 2,
            seconds: 3,
        };

        const da = new Intl.DurationFormat("da", { style: "digital" });
        expect(da.format(duration)).toBe("1.02.03");
    });

    test("locale with preferred 2-digit hours", () => {
        const duration = {
            hours: 1,
            minutes: 2,
            seconds: 3,
        };

        const lt = new Intl.DurationFormat("lt", { style: "digital" });
        expect(lt.format(duration)).toBe("01:02:03");
    });

    test("negative duration fields", () => {
        const duration = {
            years: -1,
            months: -2,
            weeks: -3,
            days: -4,
            hours: -5,
            minutes: -6,
            seconds: -7,
            milliseconds: -8,
            microseconds: -9,
            nanoseconds: -11,
        };

        const en = new Intl.DurationFormat("en", { style: "digital" });
        expect(en.format(duration)).toBe("-1 yr, 2 mths, 3 wks, 4 days, 5:06:07.008009011");

        const de = new Intl.DurationFormat("de", { style: "digital" });
        expect(de.format(duration)).toBe("-1 J, 2 Mon., 3 Wo., 4 Tg. und 5:06:07,008009011");
    });

    test("formatted fields do not have grouping separators", () => {
        const duration = {
            hours: 123456,
            minutes: 456789,
            seconds: 789123,
        };

        const en = new Intl.DurationFormat("en", { style: "digital" });
        expect(en.format(duration)).toBe("123456:456789:789123");

        const de = new Intl.DurationFormat("de", { style: "digital" });
        expect(de.format(duration)).toBe("123456:456789:789123");
    });

    test("precise mathematical values", () => {
        const en = new Intl.DurationFormat("en", { style: "digital" });

        let duration = {
            seconds: 10000000,
            nanoseconds: 1,
        };
        expect(en.format(duration)).toBe("0:00:10000000.000000001");

        duration = {
            seconds: 1,
            milliseconds: 2,
            microseconds: 3,
            nanoseconds: Number.MAX_SAFE_INTEGER,
        };
        expect(en.format(duration)).toBe("0:00:9007200.256743991");
    });

    test("format Temporal Duration objects", () => {
        const duration = Temporal.Duration.from("P1Y2M3W4DT5H6M7S");

        const en = new Intl.DurationFormat("en", { style: "digital" });
        expect(en.format(duration)).toBe("1 yr, 2 mths, 3 wks, 4 days, 5:06:07");
    });
});

describe("errors", () => {
    test("non-object duration records", () => {
        expect(() => {
            new Intl.DurationFormat().format("hello");
        }).toThrowWithMessage(RangeError, "Invalid duration string 'hello");

        [-100, Infinity, NaN, 152n, Symbol("foo"), true, null, undefined].forEach(value => {
            expect(() => {
                new Intl.DurationFormat().format(value);
            }).toThrowWithMessage(TypeError, "is not a string");
        });
    });

    test("empty duration record", () => {
        expect(() => {
            new Intl.DurationFormat().format({});
        }).toThrowWithMessage(TypeError, "Invalid duration");

        expect(() => {
            new Intl.DurationFormat().format({ foo: 123 });
        }).toThrowWithMessage(TypeError, "Invalid duration");
    });

    test("non-integral duration fields", () => {
        [
            "years",
            "months",
            "weeks",
            "days",
            "hours",
            "minutes",
            "seconds",
            "milliseconds",
            "microseconds",
            "nanoseconds",
        ].forEach(field => {
            expect(() => {
                new Intl.DurationFormat().format({ [field]: 1.5 });
            }).toThrowWithMessage(
                RangeError,
                `Invalid value for duration property '${field}': must be an integer, got 1.5`
            );

            expect(() => {
                new Intl.DurationFormat().format({ [field]: -Infinity });
            }).toThrowWithMessage(
                RangeError,
                `Invalid value for duration property '${field}': must be an integer, got -Infinity`
            );
        });
    });

    test("inconsistent field signs", () => {
        expect(() => {
            new Intl.DurationFormat().format({ years: 1, months: -1 });
        }).toThrowWithMessage(RangeError, "Invalid duration");
    });
});
