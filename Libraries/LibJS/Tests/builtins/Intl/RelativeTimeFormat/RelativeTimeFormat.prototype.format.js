describe("errors", () => {
    test("called on non-RelativeimeFormat object", () => {
        expect(() => {
            Intl.RelativeTimeFormat.prototype.format(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.RelativeTimeFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().format(Symbol.hasInstance, "year");
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Intl.RelativeTimeFormat().format(1n, "year");
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("called with unit that cannot be converted to a number", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().format(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
    });

    test("called with non-finite value", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().format(Infinity, "year");
        }).toThrowWithMessage(RangeError, "Number must not be NaN or Infinity");

        expect(() => {
            new Intl.RelativeTimeFormat().format(-Infinity, "year");
        }).toThrowWithMessage(RangeError, "Number must not be NaN or Infinity");

        expect(() => {
            new Intl.RelativeTimeFormat().format(NaN, "year");
        }).toThrowWithMessage(RangeError, "Number must not be NaN or Infinity");
    });

    test("called with non-sanctioned unit", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().format(1);
        }).toThrowWithMessage(RangeError, "Unit undefined is not a valid time unit");

        expect(() => {
            new Intl.RelativeTimeFormat().format(1, null);
        }).toThrowWithMessage(RangeError, "Unit null is not a valid time unit");

        expect(() => {
            new Intl.RelativeTimeFormat().format(1, "hello!");
        }).toThrowWithMessage(RangeError, "Unit hello! is not a valid time unit");
    });
});

let formatters = {};

["en", "ar", "pl"].forEach(locale => {
    formatters[locale] = {};

    ["long", "short", "narrow"].forEach(style => {
        formatters[locale][style] = {};

        ["always", "auto"].forEach(numeric => {
            formatters[locale][style][numeric] = new Intl.RelativeTimeFormat(locale, {
                style: style,
                numeric: numeric,
            });
        });
    });
});

function runTest(unit, style, numeric, en, ar, pl) {
    const pluralUnit = `${unit}s`;

    [-2, -1, -0, 0, 1, 2].forEach((value, i) => {
        expect(formatters["en"][style][numeric].format(value, unit)).toBe(en[i]);
        expect(formatters["ar"][style][numeric].format(value, unit)).toBe(ar[i]);
        expect(formatters["pl"][style][numeric].format(value, unit)).toBe(pl[i]);

        expect(formatters["en"][style][numeric].format(value, pluralUnit)).toBe(en[i]);
        expect(formatters["ar"][style][numeric].format(value, pluralUnit)).toBe(ar[i]);
        expect(formatters["pl"][style][numeric].format(value, pluralUnit)).toBe(pl[i]);
    });
}

describe("second", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 seconds ago", "1 second ago", "0 seconds ago", "in 0 seconds", "in 1 second", "in 2 seconds" ]; // prettier-ignore
        const ar = [ "قبل ثانيتين", "قبل ثانية واحدة", "قبل ٠ ثانية", "خلال ٠ ثانية", "خلال ثانية واحدة", "خلال ثانيتين" ]; // prettier-ignore
        const pl = [ "2 sekundy temu", "1 sekundę temu", "0 sekund temu", "za 0 sekund", "za 1 sekundę", "za 2 sekundy" ]; // prettier-ignore

        runTest("second", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 sec. ago", "1 sec. ago", "0 sec. ago", "in 0 sec.", "in 1 sec.", "in 2 sec." ]; // prettier-ignore
        const ar = [ "قبل ثانيتين", "قبل ثانية واحدة", "قبل ٠ ثانية", "خلال ٠ ثانية", "خلال ثانية واحدة", "خلال ثانيتين" ]; // prettier-ignore
        const pl = [ "2 sek. temu", "1 sek. temu", "0 sek. temu", "za 0 sek.", "za 1 sek.", "za 2 sek." ]; // prettier-ignore

        runTest("second", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2s ago", "1s ago", "0s ago", "in 0s", "in 1s", "in 2s" ]; // prettier-ignore
        const ar = [ "قبل ثانيتين", "قبل ثانية واحدة", "قبل ٠ ثانية", "خلال ٠ ثانية", "خلال ثانية واحدة", "خلال ثانيتين" ]; // prettier-ignore
        const pl = [ "2 s temu", "1 s temu", "0 s temu", "za 0 s", "za 1 s", "za 2 s" ]; // prettier-ignore

        runTest("second", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 seconds ago", "1 second ago", "now", "now", "in 1 second", "in 2 seconds" ]; // prettier-ignore
        const ar = [ "قبل ثانيتين", "قبل ثانية واحدة", "الآن", "الآن", "خلال ثانية واحدة", "خلال ثانيتين" ]; // prettier-ignore
        const pl = [ "2 sekundy temu", "1 sekundę temu", "teraz", "teraz", "za 1 sekundę", "za 2 sekundy" ]; // prettier-ignore

        runTest("second", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 sec. ago", "1 sec. ago", "now", "now", "in 1 sec.", "in 2 sec." ]; // prettier-ignore
        const ar = [ "قبل ثانيتين", "قبل ثانية واحدة", "الآن", "الآن", "خلال ثانية واحدة", "خلال ثانيتين" ]; // prettier-ignore
        const pl = [ "2 sek. temu", "1 sek. temu", "teraz", "teraz", "za 1 sek.", "za 2 sek." ]; // prettier-ignore

        runTest("second", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2s ago", "1s ago", "now", "now", "in 1s", "in 2s" ]; // prettier-ignore
        const ar = [ "قبل ثانيتين", "قبل ثانية واحدة", "الآن", "الآن", "خلال ثانية واحدة", "خلال ثانيتين" ]; // prettier-ignore
        const pl = [ "2 s temu", "1 s temu", "teraz", "teraz", "za 1 s", "za 2 s" ]; // prettier-ignore

        runTest("second", "narrow", "auto", en, ar, pl);
    });
});

describe("minute", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 minutes ago", "1 minute ago", "0 minutes ago", "in 0 minutes", "in 1 minute", "in 2 minutes" ]; // prettier-ignore
        const ar = [ "قبل دقيقتين", "قبل دقيقة واحدة", "قبل ٠ دقيقة", "خلال ٠ دقيقة", "خلال دقيقة واحدة", "خلال دقيقتين" ]; // prettier-ignore
        const pl = [ "2 minuty temu", "1 minutę temu", "0 minut temu", "za 0 minut", "za 1 minutę", "za 2 minuty" ]; // prettier-ignore

        runTest("minute", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 min. ago", "1 min. ago", "0 min. ago", "in 0 min.", "in 1 min.", "in 2 min." ]; // prettier-ignore
        const ar = [ "قبل دقيقتين", "قبل دقيقة واحدة", "قبل ٠ دقيقة", "خلال ٠ دقيقة", "خلال دقيقة واحدة", "خلال دقيقتين" ]; // prettier-ignore
        const pl = [ "2 min temu", "1 min temu", "0 min temu", "za 0 min", "za 1 min", "za 2 min" ]; // prettier-ignore

        runTest("minute", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2m ago", "1m ago", "0m ago", "in 0m", "in 1m", "in 2m" ]; // prettier-ignore
        const ar = [ "قبل دقيقتين", "قبل دقيقة واحدة", "قبل ٠ دقيقة", "خلال ٠ دقيقة", "خلال دقيقة واحدة", "خلال دقيقتين" ]; // prettier-ignore
        const pl = [ "2 min temu", "1 min temu", "0 min temu", "za 0 min", "za 1 min", "za 2 min" ]; // prettier-ignore

        runTest("minute", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 minutes ago", "1 minute ago", "this minute", "this minute", "in 1 minute", "in 2 minutes" ]; // prettier-ignore
        const ar = [ "قبل دقيقتين", "قبل دقيقة واحدة", "هذه الدقيقة", "هذه الدقيقة", "خلال دقيقة واحدة", "خلال دقيقتين" ]; // prettier-ignore
        const pl = [ "2 minuty temu", "1 minutę temu", "ta minuta", "ta minuta", "za 1 minutę", "za 2 minuty" ]; // prettier-ignore

        runTest("minute", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 min. ago", "1 min. ago", "this minute", "this minute", "in 1 min.", "in 2 min." ]; // prettier-ignore
        const ar = [ "قبل دقيقتين", "قبل دقيقة واحدة", "هذه الدقيقة", "هذه الدقيقة", "خلال دقيقة واحدة", "خلال دقيقتين" ]; // prettier-ignore
        const pl = [ "2 min temu", "1 min temu", "ta minuta", "ta minuta", "za 1 min", "za 2 min" ]; // prettier-ignore

        runTest("minute", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2m ago", "1m ago", "this minute", "this minute", "in 1m", "in 2m" ]; // prettier-ignore
        const ar = [ "قبل دقيقتين", "قبل دقيقة واحدة", "هذه الدقيقة", "هذه الدقيقة", "خلال دقيقة واحدة", "خلال دقيقتين" ]; // prettier-ignore
        const pl = [ "2 min temu", "1 min temu", "ta minuta", "ta minuta", "za 1 min", "za 2 min" ]; // prettier-ignore

        runTest("minute", "narrow", "auto", en, ar, pl);
    });
});

describe("hour", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 hours ago", "1 hour ago", "0 hours ago", "in 0 hours", "in 1 hour", "in 2 hours" ]; // prettier-ignore
        const ar = [ "قبل ساعتين", "قبل ساعة واحدة", "قبل ٠ ساعة", "خلال ٠ ساعة", "خلال ساعة واحدة", "خلال ساعتين" ]; // prettier-ignore
        const pl = [ "2 godziny temu", "1 godzinę temu", "0 godzin temu", "za 0 godzin", "za 1 godzinę", "za 2 godziny" ]; // prettier-ignore

        runTest("hour", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 hr. ago", "1 hr. ago", "0 hr. ago", "in 0 hr.", "in 1 hr.", "in 2 hr." ]; // prettier-ignore
        const ar = [ "قبل ساعتين", "قبل ساعة واحدة", "قبل ٠ ساعة", "خلال ٠ ساعة", "خلال ساعة واحدة", "خلال ساعتين" ]; // prettier-ignore
        const pl = [ "2 godz. temu", "1 godz. temu", "0 godz. temu", "za 0 godz.", "za 1 godz.", "za 2 godz." ]; // prettier-ignore

        runTest("hour", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2h ago", "1h ago", "0h ago", "in 0h", "in 1h", "in 2h" ]; // prettier-ignore
        const ar = [ "قبل ساعتين", "قبل ساعة واحدة", "قبل ٠ ساعة", "خلال ٠ ساعة", "خلال ساعة واحدة", "خلال ساعتين" ]; // prettier-ignore
        const pl = [ "2 g. temu", "1 g. temu", "0 g. temu", "za 0 g.", "za 1 g.", "za 2 g." ]; // prettier-ignore

        runTest("hour", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 hours ago", "1 hour ago", "this hour", "this hour", "in 1 hour", "in 2 hours" ]; // prettier-ignore
        const ar = [ "قبل ساعتين", "قبل ساعة واحدة", "الساعة الحالية", "الساعة الحالية", "خلال ساعة واحدة", "خلال ساعتين" ]; // prettier-ignore
        const pl = [ "2 godziny temu", "1 godzinę temu", "ta godzina", "ta godzina", "za 1 godzinę", "za 2 godziny" ]; // prettier-ignore

        runTest("hour", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 hr. ago", "1 hr. ago", "this hour", "this hour", "in 1 hr.", "in 2 hr." ]; // prettier-ignore
        const ar = [ "قبل ساعتين", "قبل ساعة واحدة", "الساعة الحالية", "الساعة الحالية", "خلال ساعة واحدة", "خلال ساعتين" ]; // prettier-ignore
        const pl = [ "2 godz. temu", "1 godz. temu", "ta godzina", "ta godzina", "za 1 godz.", "za 2 godz." ]; // prettier-ignore

        runTest("hour", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2h ago", "1h ago", "this hour", "this hour", "in 1h", "in 2h" ]; // prettier-ignore
        const ar = [ "قبل ساعتين", "قبل ساعة واحدة", "الساعة الحالية", "الساعة الحالية", "خلال ساعة واحدة", "خلال ساعتين" ]; // prettier-ignore
        const pl = [ "2 g. temu", "1 g. temu", "ta godzina", "ta godzina", "za 1 g.", "za 2 g." ]; // prettier-ignore

        runTest("hour", "narrow", "auto", en, ar, pl);
    });
});

describe("day", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 days ago", "1 day ago", "0 days ago", "in 0 days", "in 1 day", "in 2 days" ]; // prettier-ignore
        const ar = [ "قبل يومين", "قبل يوم واحد", "قبل ٠ يوم", "خلال ٠ يوم", "خلال يوم واحد", "خلال يومين" ]; // prettier-ignore
        const pl = [ "2 dni temu", "1 dzień temu", "0 dni temu", "za 0 dni", "za 1 dzień", "za 2 dni" ]; // prettier-ignore

        runTest("day", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 days ago", "1 day ago", "0 days ago", "in 0 days", "in 1 day", "in 2 days" ]; // prettier-ignore
        const ar = [ "قبل يومين", "قبل يوم واحد", "قبل ٠ يوم", "خلال ٠ يوم", "خلال يوم واحد", "خلال يومين" ]; // prettier-ignore
        const pl = [ "2 dni temu", "1 dzień temu", "0 dni temu", "za 0 dni", "za 1 dzień", "za 2 dni" ]; // prettier-ignore

        runTest("day", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2d ago", "1d ago", "0d ago", "in 0d", "in 1d", "in 2d" ]; // prettier-ignore
        const ar = [ "قبل يومين", "قبل يوم واحد", "قبل ٠ يوم", "خلال ٠ يوم", "خلال يوم واحد", "خلال يومين" ]; // prettier-ignore
        const pl = [ "2 dni temu", "1 dzień temu", "0 dni temu", "za 0 dni", "za 1 dzień", "za 2 dni" ]; // prettier-ignore

        runTest("day", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 days ago", "yesterday", "today", "today", "tomorrow", "in 2 days" ]; // prettier-ignore
        const ar = [ "أول أمس", "أمس", "اليوم", "اليوم", "غدًا", "بعد الغد" ]; // prettier-ignore
        const pl = [ "przedwczoraj", "wczoraj", "dzisiaj", "dzisiaj", "jutro", "pojutrze" ]; // prettier-ignore

        runTest("day", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 days ago", "yesterday", "today", "today", "tomorrow", "in 2 days" ]; // prettier-ignore
        const ar = [ "أول أمس", "أمس", "اليوم", "اليوم", "غدًا", "بعد الغد" ]; // prettier-ignore
        const pl = [ "przedwczoraj", "wczoraj", "dzisiaj", "dzisiaj", "jutro", "pojutrze" ]; // prettier-ignore

        runTest("day", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2d ago", "yesterday", "today", "today", "tomorrow", "in 2d" ]; // prettier-ignore
        const ar = [ "أول أمس", "أمس", "اليوم", "اليوم", "غدًا", "بعد الغد" ]; // prettier-ignore
        const pl = [ "przedwczoraj", "wcz.", "dziś", "dziś", "jutro", "pojutrze" ]; // prettier-ignore

        runTest("day", "narrow", "auto", en, ar, pl);
    });
});

describe("week", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 weeks ago", "1 week ago", "0 weeks ago", "in 0 weeks", "in 1 week", "in 2 weeks" ]; // prettier-ignore
        const ar = [ "قبل أسبوعين", "قبل أسبوع واحد", "قبل ٠ أسبوع", "خلال ٠ أسبوع", "خلال أسبوع واحد", "خلال أسبوعين" ]; // prettier-ignore
        const pl = [ "2 tygodnie temu", "1 tydzień temu", "0 tygodni temu", "za 0 tygodni", "za 1 tydzień", "za 2 tygodnie" ]; // prettier-ignore

        runTest("week", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 wk. ago", "1 wk. ago", "0 wk. ago", "in 0 wk.", "in 1 wk.", "in 2 wk." ]; // prettier-ignore
        const ar = [ "قبل أسبوعين", "قبل أسبوع واحد", "قبل ٠ أسبوع", "خلال ٠ أسبوع", "خلال أسبوع واحد", "خلال ٢ أسبوعين" ]; // prettier-ignore
        const pl = [ "2 tyg. temu", "1 tydz. temu", "0 tyg. temu", "za 0 tyg.", "za 1 tydz.", "za 2 tyg." ]; // prettier-ignore

        runTest("week", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2w ago", "1w ago", "0w ago", "in 0w", "in 1w", "in 2w" ]; // prettier-ignore
        const ar = [ "قبل أسبوعين", "قبل أسبوع واحد", "قبل ٠ أسبوع", "خلال ٠ أسبوع", "خلال أسبوع واحد", "خلال أسبوعين" ]; // prettier-ignore
        const pl = [ "2 tyg. temu", "1 tydz. temu", "0 tyg. temu", "za 0 tyg.", "za 1 tydz.", "za 2 tyg." ]; // prettier-ignore

        runTest("week", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 weeks ago", "last week", "this week", "this week", "next week", "in 2 weeks" ]; // prettier-ignore
        const ar = [ "قبل أسبوعين", "الأسبوع الماضي", "هذا الأسبوع", "هذا الأسبوع", "الأسبوع القادم", "خلال أسبوعين" ]; // prettier-ignore
        const pl = [ "2 tygodnie temu", "w zeszłym tygodniu", "w tym tygodniu", "w tym tygodniu", "w przyszłym tygodniu", "za 2 tygodnie" ]; // prettier-ignore

        runTest("week", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 wk. ago", "last wk.", "this wk.", "this wk.", "next wk.", "in 2 wk." ]; // prettier-ignore
        const ar = [ "قبل أسبوعين", "الأسبوع الماضي", "هذا الأسبوع", "هذا الأسبوع", "الأسبوع القادم", "خلال ٢ أسبوعين" ]; // prettier-ignore
        const pl = [ "2 tyg. temu", "w zeszłym tyg.", "w tym tyg.", "w tym tyg.", "w przyszłym tyg.", "za 2 tyg." ]; // prettier-ignore

        runTest("week", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2w ago", "last wk.", "this wk.", "this wk.", "next wk.", "in 2w" ]; // prettier-ignore
        const ar = [ "قبل أسبوعين", "الأسبوع الماضي", "هذا الأسبوع", "هذا الأسبوع", "الأسبوع القادم", "خلال أسبوعين" ]; // prettier-ignore
        const pl = [ "2 tyg. temu", "w zeszłym tyg.", "w tym tyg.", "w tym tyg.", "w przyszłym tyg.", "za 2 tyg." ]; // prettier-ignore

        runTest("week", "narrow", "auto", en, ar, pl);
    });
});

describe("month", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 months ago", "1 month ago", "0 months ago", "in 0 months", "in 1 month", "in 2 months" ]; // prettier-ignore
        const ar = [ "قبل شهرين", "قبل شهر واحد", "قبل ٠ شهر", "خلال ٠ شهر", "خلال شهر واحد", "خلال شهرين" ]; // prettier-ignore
        const pl = [ "2 miesiące temu", "1 miesiąc temu", "0 miesięcy temu", "za 0 miesięcy", "za 1 miesiąc", "za 2 miesiące" ]; // prettier-ignore

        runTest("month", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 mo. ago", "1 mo. ago", "0 mo. ago", "in 0 mo.", "in 1 mo.", "in 2 mo." ]; // prettier-ignore
        const ar = [ "قبل شهرين", "قبل شهر واحد", "قبل ٠ شهر", "خلال ٠ شهر", "خلال شهر واحد", "خلال شهرين" ]; // prettier-ignore
        const pl = [ "2 mies. temu", "1 mies. temu", "0 mies. temu", "za 0 mies.", "za 1 mies.", "za 2 mies." ]; // prettier-ignore

        runTest("month", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2mo ago", "1mo ago", "0mo ago", "in 0mo", "in 1mo", "in 2mo" ]; // prettier-ignore
        const ar = [ "قبل شهرين", "قبل شهر واحد", "قبل ٠ شهر", "خلال ٠ شهر", "خلال شهر واحد", "خلال شهرين" ]; // prettier-ignore
        const pl = [ "2 mies. temu", "1 mies. temu", "0 mies. temu", "za 0 mies.", "za 1 mies.", "za 2 mies." ]; // prettier-ignore

        runTest("month", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 months ago", "last month", "this month", "this month", "next month", "in 2 months" ]; // prettier-ignore
        const ar = [ "قبل شهرين", "الشهر الماضي", "هذا الشهر", "هذا الشهر", "الشهر القادم", "خلال شهرين" ]; // prettier-ignore
        const pl = [ "2 miesiące temu", "w zeszłym miesiącu", "w tym miesiącu", "w tym miesiącu", "w przyszłym miesiącu", "za 2 miesiące" ]; // prettier-ignore

        runTest("month", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 mo. ago", "last mo.", "this mo.", "this mo.", "next mo.", "in 2 mo." ]; // prettier-ignore
        const ar = [ "قبل شهرين", "الشهر الماضي", "هذا الشهر", "هذا الشهر", "الشهر القادم", "خلال شهرين" ]; // prettier-ignore
        const pl = [ "2 mies. temu", "w zeszłym mies.", "w tym mies.", "w tym mies.", "w przyszłym mies.", "za 2 mies." ]; // prettier-ignore

        runTest("month", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2mo ago", "last mo.", "this mo.", "this mo.", "next mo.", "in 2mo" ]; // prettier-ignore
        const ar = [ "قبل شهرين", "الشهر الماضي", "هذا الشهر", "هذا الشهر", "الشهر القادم", "خلال شهرين" ]; // prettier-ignore
        const pl = [ "2 mies. temu", "w zeszłym mies.", "w tym mies.", "w tym mies.", "w przyszłym mies.", "za 2 mies." ]; // prettier-ignore

        runTest("month", "narrow", "auto", en, ar, pl);
    });
});

describe("quarter", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 quarters ago", "1 quarter ago", "0 quarters ago", "in 0 quarters", "in 1 quarter", "in 2 quarters" ]; // prettier-ignore
        const ar = [ "قبل ربعي سنة", "قبل ربع سنة واحد", "قبل ٠ ربع سنة", "خلال ٠ ربع سنة", "خلال ربع سنة واحد", "خلال ربعي سنة" ]; // prettier-ignore
        const pl = [ "2 kwartały temu", "1 kwartał temu", "0 kwartałów temu", "za 0 kwartałów", "za 1 kwartał", "za 2 kwartały" ]; // prettier-ignore

        runTest("quarter", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 qtrs. ago", "1 qtr. ago", "0 qtrs. ago", "in 0 qtrs.", "in 1 qtr.", "in 2 qtrs." ]; // prettier-ignore
        const ar = [ "قبل ربعي سنة", "قبل ربع سنة واحد", "قبل ٠ ربع سنة", "خلال ٠ ربع سنة", "خلال ربع سنة واحد", "خلال ربعي سنة" ]; // prettier-ignore
        const pl = [ "2 kw. temu", "1 kw. temu", "0 kw. temu", "za 0 kw.", "za 1 kw.", "za 2 kw." ]; // prettier-ignore

        runTest("quarter", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2q ago", "1q ago", "0q ago", "in 0q", "in 1q", "in 2q" ]; // prettier-ignore
        const ar = [ "قبل ربعي سنة", "قبل ربع سنة واحد", "قبل ٠ ربع سنة", "خلال ٠ ربع سنة", "خلال ربع سنة واحد", "خلال ربعي سنة" ]; // prettier-ignore
        const pl = [ "2 kw. temu", "1 kw. temu", "0 kw. temu", "za 0 kw.", "za 1 kw.", "za 2 kw." ]; // prettier-ignore

        runTest("quarter", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 quarters ago", "last quarter", "this quarter", "this quarter", "next quarter", "in 2 quarters" ]; // prettier-ignore
        const ar = [ "قبل ربعي سنة", "الربع الأخير", "هذا الربع", "هذا الربع", "الربع القادم", "خلال ربعي سنة" ]; // prettier-ignore
        const pl = [ "2 kwartały temu", "w zeszłym kwartale", "w tym kwartale", "w tym kwartale", "w przyszłym kwartale", "za 2 kwartały" ]; // prettier-ignore

        runTest("quarter", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 qtrs. ago", "last qtr.", "this qtr.", "this qtr.", "next qtr.", "in 2 qtrs." ]; // prettier-ignore
        const ar = [ "قبل ربعي سنة", "الربع الأخير", "هذا الربع", "هذا الربع", "الربع القادم", "خلال ربعي سنة" ]; // prettier-ignore
        const pl = [ "2 kw. temu", "w zeszłym kwartale", "w tym kwartale", "w tym kwartale", "w przyszłym kwartale", "za 2 kw." ]; // prettier-ignore

        runTest("quarter", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2q ago", "last qtr.", "this qtr.", "this qtr.", "next qtr.", "in 2q" ]; // prettier-ignore
        const ar = [ "قبل ربعي سنة", "الربع الأخير", "هذا الربع", "هذا الربع", "الربع القادم", "خلال ربعي سنة" ]; // prettier-ignore
        const pl = [ "2 kw. temu", "w zeszłym kwartale", "w tym kwartale", "w tym kwartale", "w przyszłym kwartale", "za 2 kw." ]; // prettier-ignore

        runTest("quarter", "narrow", "auto", en, ar, pl);
    });
});

describe("year", () => {
    test("style=long, numeric=always", () => {
        const en = [ "2 years ago", "1 year ago", "0 years ago", "in 0 years", "in 1 year", "in 2 years" ]; // prettier-ignore
        const ar = [ "قبل سنتين", "قبل سنة واحدة", "قبل ٠ سنة", "خلال ٠ سنة", "خلال سنة واحدة", "خلال سنتين" ]; // prettier-ignore
        const pl = [ "2 lata temu", "1 rok temu", "0 lat temu", "za 0 lat", "za 1 rok", "za 2 lata" ]; // prettier-ignore

        runTest("year", "long", "always", en, ar, pl);
    });

    test("style=short, numeric=always", () => {
        const en = [ "2 yr. ago", "1 yr. ago", "0 yr. ago", "in 0 yr.", "in 1 yr.", "in 2 yr." ]; // prettier-ignore
        const ar = [ "قبل سنتين", "قبل سنة واحدة", "قبل ٠ سنة", "خلال ٠ سنة", "خلال سنة واحدة", "خلال سنتين" ]; // prettier-ignore
        const pl = [ "2 lata temu", "1 rok temu", "0 lat temu", "za 0 lat", "za 1 rok", "za 2 lata" ]; // prettier-ignore

        runTest("year", "short", "always", en, ar, pl);
    });

    test("style=narrow, numeric=always", () => {
        const en = [ "2y ago", "1y ago", "0y ago", "in 0y", "in 1y", "in 2y" ]; // prettier-ignore
        const ar = [ "قبل سنتين", "قبل سنة واحدة", "قبل ٠ سنة", "خلال ٠ سنة", "خلال سنة واحدة", "خلال سنتين" ]; // prettier-ignore
        const pl = [ "2 lata temu", "1 rok temu", "0 lat temu", "za 0 lat", "za 1 rok", "za 2 lata" ]; // prettier-ignore

        runTest("year", "narrow", "always", en, ar, pl);
    });

    test("style=long, numeric=auto", () => {
        const en = [ "2 years ago", "last year", "this year", "this year", "next year", "in 2 years" ]; // prettier-ignore
        const ar = [ "قبل سنتين", "السنة الماضية", "السنة الحالية", "السنة الحالية", "السنة القادمة", "خلال سنتين" ]; // prettier-ignore
        const pl = [ "2 lata temu", "w zeszłym roku", "w tym roku", "w tym roku", "w przyszłym roku", "za 2 lata" ]; // prettier-ignore

        runTest("year", "long", "auto", en, ar, pl);
    });

    test("style=short, numeric=auto", () => {
        const en = [ "2 yr. ago", "last yr.", "this yr.", "this yr.", "next yr.", "in 2 yr." ]; // prettier-ignore
        const ar = [ "قبل سنتين", "السنة الماضية", "السنة الحالية", "السنة الحالية", "السنة القادمة", "خلال سنتين" ]; // prettier-ignore
        const pl = [ "2 lata temu", "w zeszłym roku", "w tym roku", "w tym roku", "w przyszłym roku", "za 2 lata" ]; // prettier-ignore

        runTest("year", "short", "auto", en, ar, pl);
    });

    test("style=narrow, numeric=auto", () => {
        const en = [ "2y ago", "last yr.", "this yr.", "this yr.", "next yr.", "in 2y" ]; // prettier-ignore
        const ar = [ "قبل سنتين", "السنة الماضية", "السنة الحالية", "السنة الحالية", "السنة القادمة", "خلال سنتين" ]; // prettier-ignore
        const pl = [ "2 lata temu", "w zeszłym roku", "w tym roku", "w tym roku", "w przyszłym roku", "za 2 lata" ]; // prettier-ignore

        runTest("year", "narrow", "auto", en, ar, pl);
    });
});
