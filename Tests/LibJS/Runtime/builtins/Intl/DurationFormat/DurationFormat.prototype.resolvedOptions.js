describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.DurationFormat.prototype.resolvedOptions).toHaveLength(0);
    });

    test("locale only contains relevant extension keys", () => {
        const en1 = new Intl.DurationFormat("en-u-ca-islamicc");
        expect(en1.resolvedOptions().locale).toBe("en");

        const en3 = new Intl.DurationFormat("en-u-ca-islamicc-nu-latn");
        expect(en3.resolvedOptions().locale).toBe("en-u-nu-latn");
    });

    test("numberingSystem may be set by option", () => {
        const en = new Intl.DurationFormat("en", { numberingSystem: "latn" });
        expect(en.resolvedOptions().numberingSystem).toBe("latn");

        const el = new Intl.DurationFormat("el", { numberingSystem: "latn" });
        expect(el.resolvedOptions().numberingSystem).toBe("latn");
    });

    test("numberingSystem may be set by locale extension", () => {
        const en = new Intl.DurationFormat("en-u-nu-latn");
        expect(en.resolvedOptions().numberingSystem).toBe("latn");

        const el = new Intl.DurationFormat("el-u-nu-latn");
        expect(el.resolvedOptions().numberingSystem).toBe("latn");
    });

    test("numberingSystem option overrides locale extension", () => {
        const el = new Intl.DurationFormat("el-u-nu-latn", { numberingSystem: "adlm" });
        expect(el.resolvedOptions().numberingSystem).toBe("adlm");
    });

    test("numberingSystem option limited to known 'nu' values", () => {
        ["latn", "foo"].forEach(numberingSystem => {
            const en = new Intl.DurationFormat("en", { numberingSystem: numberingSystem });
            expect(en.resolvedOptions().numberingSystem).toBe("latn");
        });

        ["latn", "foo"].forEach(numberingSystem => {
            const en = new Intl.DurationFormat(`en-u-nu-${numberingSystem}`);
            expect(en.resolvedOptions().numberingSystem).toBe("latn");
        });

        ["latn", "adlm"].forEach(numberingSystem => {
            const el = new Intl.DurationFormat("el", { numberingSystem: numberingSystem });
            expect(el.resolvedOptions().numberingSystem).toBe(numberingSystem);
        });

        ["latn", "adlm"].forEach(numberingSystem => {
            const el = new Intl.DurationFormat(`el-u-nu-${numberingSystem}`);
            expect(el.resolvedOptions().numberingSystem).toBe(numberingSystem);
        });
    });

    test("style", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().style).toBe("short");

        ["long", "short", "narrow", "digital"].forEach(style => {
            const en2 = new Intl.DurationFormat("en", { style: style });
            expect(en2.resolvedOptions().style).toBe(style);
        });
    });

    test("years", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().years).toBe("short");

        ["long", "short", "narrow"].forEach(years => {
            const en2 = new Intl.DurationFormat("en", { years: years });
            expect(en2.resolvedOptions().years).toBe(years);
        });
    });

    test("yearsDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().yearsDisplay).toBe("auto");

        ["always", "auto"].forEach(yearsDisplay => {
            const en2 = new Intl.DurationFormat("en", { yearsDisplay: yearsDisplay });
            expect(en2.resolvedOptions().yearsDisplay).toBe(yearsDisplay);
        });
    });

    test("months", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().months).toBe("short");

        ["long", "short", "narrow"].forEach(months => {
            const en2 = new Intl.DurationFormat("en", { months: months });
            expect(en2.resolvedOptions().months).toBe(months);
        });
    });

    test("monthsDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().monthsDisplay).toBe("auto");

        ["always", "auto"].forEach(monthsDisplay => {
            const en2 = new Intl.DurationFormat("en", { monthsDisplay: monthsDisplay });
            expect(en2.resolvedOptions().monthsDisplay).toBe(monthsDisplay);
        });
    });

    test("weeks", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().weeks).toBe("short");

        ["long", "short", "narrow"].forEach(weeks => {
            const en2 = new Intl.DurationFormat("en", { weeks: weeks });
            expect(en2.resolvedOptions().weeks).toBe(weeks);
        });
    });

    test("weeksDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().weeksDisplay).toBe("auto");

        ["always", "auto"].forEach(weeksDisplay => {
            const en2 = new Intl.DurationFormat("en", { weeksDisplay: weeksDisplay });
            expect(en2.resolvedOptions().weeksDisplay).toBe(weeksDisplay);
        });
    });

    test("days", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().days).toBe("short");

        ["long", "short", "narrow"].forEach(days => {
            const en2 = new Intl.DurationFormat("en", { days: days });
            expect(en2.resolvedOptions().days).toBe(days);
        });
    });

    test("daysDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().daysDisplay).toBe("auto");

        ["always", "auto"].forEach(daysDisplay => {
            const en2 = new Intl.DurationFormat("en", { daysDisplay: daysDisplay });
            expect(en2.resolvedOptions().daysDisplay).toBe(daysDisplay);
        });
    });

    test("hours", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().hours).toBe("short");

        ["long", "short", "narrow"].forEach(hours => {
            const en2 = new Intl.DurationFormat("en", { hours: hours });
            expect(en2.resolvedOptions().hours).toBe(hours);
        });
        ["numeric", "2-digit"].forEach(hours => {
            const en2 = new Intl.DurationFormat("en", { style: "digital", hours: hours });
            expect(en2.resolvedOptions().hours).toBe(hours);
        });
    });

    test("hoursDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().hoursDisplay).toBe("auto");

        ["always", "auto"].forEach(hoursDisplay => {
            const en2 = new Intl.DurationFormat("en", { hoursDisplay: hoursDisplay });
            expect(en2.resolvedOptions().hoursDisplay).toBe(hoursDisplay);
        });
    });

    test("minutes", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().minutes).toBe("short");

        ["long", "short", "narrow"].forEach(minutes => {
            const en2 = new Intl.DurationFormat("en", { minutes: minutes });
            expect(en2.resolvedOptions().minutes).toBe(minutes);
        });
        ["numeric", "2-digit"].forEach(minutes => {
            const en2 = new Intl.DurationFormat("en", { style: "digital", minutes: minutes });
            expect(en2.resolvedOptions().minutes).toBe("2-digit");
        });
    });

    test("minutesDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().minutesDisplay).toBe("auto");

        ["always", "auto"].forEach(minutesDisplay => {
            const en2 = new Intl.DurationFormat("en", { minutesDisplay: minutesDisplay });
            expect(en2.resolvedOptions().minutesDisplay).toBe(minutesDisplay);
        });
    });

    test("seconds", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().seconds).toBe("short");

        ["long", "short", "narrow"].forEach(seconds => {
            const en2 = new Intl.DurationFormat("en", { seconds: seconds });
            expect(en2.resolvedOptions().seconds).toBe(seconds);
        });
        ["numeric", "2-digit"].forEach(seconds => {
            const en2 = new Intl.DurationFormat("en", { style: "digital", seconds: seconds });
            expect(en2.resolvedOptions().seconds).toBe("2-digit");
        });
    });

    test("secondsDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().secondsDisplay).toBe("auto");

        ["always", "auto"].forEach(secondsDisplay => {
            const en2 = new Intl.DurationFormat("en", { secondsDisplay: secondsDisplay });
            expect(en2.resolvedOptions().secondsDisplay).toBe(secondsDisplay);
        });
    });

    test("milliseconds", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().milliseconds).toBe("short");

        ["long", "short", "narrow"].forEach(milliseconds => {
            const en2 = new Intl.DurationFormat("en", { milliseconds: milliseconds });
            expect(en2.resolvedOptions().milliseconds).toBe(milliseconds);
        });
        const en2 = new Intl.DurationFormat("en", { style: "digital", milliseconds: "numeric" });
        expect(en2.resolvedOptions().milliseconds).toBe("numeric");
    });

    test("millisecondsDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().millisecondsDisplay).toBe("auto");

        ["always", "auto"].forEach(millisecondsDisplay => {
            const en2 = new Intl.DurationFormat("en", { millisecondsDisplay: millisecondsDisplay });
            expect(en2.resolvedOptions().millisecondsDisplay).toBe(millisecondsDisplay);
        });
    });

    test("microseconds", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().microseconds).toBe("short");

        ["long", "short", "narrow"].forEach(microseconds => {
            const en2 = new Intl.DurationFormat("en", { microseconds: microseconds });
            expect(en2.resolvedOptions().microseconds).toBe(microseconds);
        });
        const en2 = new Intl.DurationFormat("en", { style: "digital", microseconds: "numeric" });
        expect(en2.resolvedOptions().microseconds).toBe("numeric");
    });

    test("microsecondsDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().microsecondsDisplay).toBe("auto");

        ["always", "auto"].forEach(microsecondsDisplay => {
            const en2 = new Intl.DurationFormat("en", { microsecondsDisplay: microsecondsDisplay });
            expect(en2.resolvedOptions().microsecondsDisplay).toBe(microsecondsDisplay);
        });
    });

    test("nanoseconds", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().nanoseconds).toBe("short");

        ["long", "short", "narrow", "numeric"].forEach(nanoseconds => {
            const en2 = new Intl.DurationFormat("en", { nanoseconds: nanoseconds });
            expect(en2.resolvedOptions().nanoseconds).toBe(nanoseconds);
        });
        const en2 = new Intl.DurationFormat("en", { style: "digital", nanoseconds: "numeric" });
        expect(en2.resolvedOptions().nanoseconds).toBe("numeric");
    });

    test("nanosecondsDisplay", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().nanosecondsDisplay).toBe("auto");

        ["always", "auto"].forEach(nanosecondsDisplay => {
            const en2 = new Intl.DurationFormat("en", { nanosecondsDisplay: nanosecondsDisplay });
            expect(en2.resolvedOptions().nanosecondsDisplay).toBe(nanosecondsDisplay);
        });
    });

    test("fractionalDigits", () => {
        const en1 = new Intl.DurationFormat("en");
        expect(en1.resolvedOptions().fractionalDigits).toBeUndefined();

        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].forEach(fractionalDigits => {
            const en2 = new Intl.DurationFormat("en", { fractionalDigits: fractionalDigits });
            expect(en2.resolvedOptions().fractionalDigits).toBe(fractionalDigits);
        });
    });
});
