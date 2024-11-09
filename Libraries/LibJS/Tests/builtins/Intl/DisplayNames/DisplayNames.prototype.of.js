describe("errors", () => {
    test("invalid language", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "language" }).of("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option language");
    });

    test("invalid region", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "region" }).of("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option region");
    });

    test("invalid script", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "script" }).of("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option script");
    });

    test("invalid currency", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "currency" }).of("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option currency");
    });

    test("invalid calendar", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "calendar" }).of("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option calendar");

        expect(() => {
            new Intl.DisplayNames("en", { type: "calendar" }).of("abc_def");
        }).toThrowWithMessage(RangeError, "abc_def is not a valid value for option calendar");
    });

    test("invalid dateTimeField", () => {
        expect(() => {
            new Intl.DisplayNames("en", { type: "dateTimeField" }).of("hello!");
        }).toThrowWithMessage(RangeError, "hello! is not a valid value for option dateTimeField");
    });
});

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Intl.DisplayNames.prototype.of).toHaveLength(1);
    });

    test("option type language, display dialect", () => {
        // prettier-ignore
        const data = [
            { locale: "en", en: "English", es419: "inglés", zhHant: "英文" },
            { locale: "en-US", en: "American English", es419: "inglés estadounidense", zhHant: "英文（美國）" },
            { locale: "en-GB", en: "British English", es419: "inglés británico", zhHant: "英文（英國）" },
            { locale: "sr", en: "Serbian", es419: "serbio", zhHant: "塞爾維亞文" },
            { locale: "sr-Cyrl", en: "Serbian (Cyrillic)", es419: "serbio (cirílico)", zhHant: "塞爾維亞文（西里爾文字）" },
            { locale: "sr-Cyrl-BA", en: "Serbian (Cyrillic, Bosnia & Herzegovina)", es419: "serbio (cirílico, Bosnia-Herzegovina)", zhHant: "塞爾維亞文（西里爾文字，波士尼亞與赫塞哥維納）" },
        ];

        const en = new Intl.DisplayNames("en", { type: "language", languageDisplay: "dialect" });
        const es419 = new Intl.DisplayNames("es-419", {
            type: "language",
            languageDisplay: "dialect",
        });
        const zhHant = new Intl.DisplayNames("zh-Hant", {
            type: "language",
            languageDisplay: "dialect",
        });

        data.forEach(d => {
            expect(en.of(d.locale)).toBe(d.en);
            expect(es419.of(d.locale)).toBe(d.es419);
            expect(zhHant.of(d.locale)).toBe(d.zhHant);
        });
    });

    test("option type language, display standard", () => {
        // prettier-ignore
        const data = [
            { locale: "en", en: "English", es419: "inglés", zhHant: "英文" },
            { locale: "en-US", en: "English (United States)", es419: "inglés (Estados Unidos)", zhHant: "英文（美國）" },
            { locale: "en-GB", en: "English (United Kingdom)", es419: "inglés (Reino Unido)", zhHant: "英文（英國）" },
            { locale: "sr", en: "Serbian", es419: "serbio", zhHant: "塞爾維亞文" },
            { locale: "sr-Cyrl", en: "Serbian (Cyrillic)", es419: "serbio (cirílico)", zhHant: "塞爾維亞文（西里爾文字）" },
            { locale: "sr-Cyrl-BA", en: "Serbian (Cyrillic, Bosnia & Herzegovina)", es419: "serbio (cirílico, Bosnia-Herzegovina)", zhHant: "塞爾維亞文（西里爾文字，波士尼亞與赫塞哥維納）" },
        ];

        const en = new Intl.DisplayNames("en", { type: "language", languageDisplay: "standard" });
        const es419 = new Intl.DisplayNames("es-419", {
            type: "language",
            languageDisplay: "standard",
        });
        const zhHant = new Intl.DisplayNames("zh-Hant", {
            type: "language",
            languageDisplay: "standard",
        });

        data.forEach(d => {
            expect(en.of(d.locale)).toBe(d.en);
            expect(es419.of(d.locale)).toBe(d.es419);
            expect(zhHant.of(d.locale)).toBe(d.zhHant);
        });
    });

    test("option type region", () => {
        const en = new Intl.DisplayNames("en", { type: "region" });
        expect(en.of("US")).toBe("United States");

        const es419 = new Intl.DisplayNames("es-419", { type: "region" });
        expect(es419.of("US")).toBe("Estados Unidos");

        const zhHant = new Intl.DisplayNames(["zh-Hant"], { type: "region" });
        expect(zhHant.of("US")).toBe("美國");

        expect(en.of("AA")).toBe("AA");
        expect(es419.of("AA")).toBe("AA");
        expect(zhHant.of("AA")).toBe("AA");
    });

    test("option type script", () => {
        const en = new Intl.DisplayNames("en", { type: "script" });
        expect(en.of("Latn")).toBe("Latin");

        const es419 = new Intl.DisplayNames("es-419", { type: "script" });
        expect(es419.of("Latn")).toBe("latín");

        const zhHant = new Intl.DisplayNames(["zh-Hant"], { type: "script" });
        expect(zhHant.of("Latn")).toBe("拉丁文");

        expect(en.of("Aaaa")).toBe("Aaaa");
        expect(es419.of("Aaaa")).toBe("Aaaa");
        expect(zhHant.of("Aaaa")).toBe("Aaaa");
    });

    test("option type currency, style long", () => {
        const en = new Intl.DisplayNames("en", { type: "currency", style: "long" });
        expect(en.of("USD")).toBe("US Dollar");

        const es419 = new Intl.DisplayNames("es-419", { type: "currency", style: "long" });
        expect(es419.of("USD")).toBe("dólar estadounidense");

        const zhHant = new Intl.DisplayNames(["zh-Hant"], { type: "currency", style: "long" });
        expect(zhHant.of("USD")).toBe("美元");

        expect(en.of("AAA")).toBe("AAA");
        expect(es419.of("AAA")).toBe("AAA");
        expect(zhHant.of("AAA")).toBe("AAA");
    });

    test("option type currency, style short", () => {
        const en = new Intl.DisplayNames("en", { type: "currency", style: "short" });
        expect(en.of("USD")).toBe("$");

        const es419 = new Intl.DisplayNames("es-419", { type: "currency", style: "short" });
        expect(es419.of("USD")).toBe("USD");

        const zhHant = new Intl.DisplayNames(["zh-Hant"], { type: "currency", style: "short" });
        expect(zhHant.of("USD")).toBe("US$");

        expect(en.of("AAA")).toBe("AAA");
        expect(es419.of("AAA")).toBe("AAA");
        expect(zhHant.of("AAA")).toBe("AAA");
    });

    test("option type currency, style narrow", () => {
        const en = new Intl.DisplayNames("en", { type: "currency", style: "narrow" });
        expect(en.of("USD")).toBe("$");

        const es419 = new Intl.DisplayNames("es-419", { type: "currency", style: "narrow" });
        expect(es419.of("USD")).toBe("$");

        const zhHant = new Intl.DisplayNames(["zh-Hant"], { type: "currency", style: "narrow" });
        expect(zhHant.of("USD")).toBe("$");

        expect(en.of("AAA")).toBe("AAA");
        expect(es419.of("AAA")).toBe("AAA");
        expect(zhHant.of("AAA")).toBe("AAA");
    });

    test("option type calendar", () => {
        // prettier-ignore
        const data = [
            { calendar: "buddhist", en: "Buddhist Calendar", es419: "calendario budista", zhHant: "佛曆" },
            { calendar: "chinese", en: "Chinese Calendar", es419: "calendario chino", zhHant: "農曆" },
            { calendar: "coptic", en: "Coptic Calendar", es419: "calendario cóptico", zhHant: "科普特曆" },
            { calendar: "dangi", en: "Dangi Calendar", es419: "calendario dangi", zhHant: "檀紀曆" },
            { calendar: "ethioaa", en: "Ethiopic Amete Alem Calendar", es419: "calendario etíope Amete Alem", zhHant: "衣索比亞曆 (Amete Alem)" },
            { calendar: "ethiopic", en: "Ethiopic Calendar", es419: "calendario etíope", zhHant: "衣索比亞曆" },
            { calendar: "gregory", en: "Gregorian Calendar", es419: "calendario gregoriano", zhHant: "公曆" },
            { calendar: "hebrew", en: "Hebrew Calendar", es419: "calendario hebreo", zhHant: "希伯來曆" },
            { calendar: "indian", en: "Indian National Calendar", es419: "calendario nacional hindú", zhHant: "印度國曆" },
            { calendar: "islamic", en: "Hijri Calendar", es419: "calendario hijri", zhHant: "伊斯蘭曆" },
            { calendar: "islamic-civil", en: "Hijri Calendar (tabular, civil epoch)", es419: "calendario hijri tabular", zhHant: "伊斯蘭民用曆" },
            { calendar: "islamic-umalqura", en: "Hijri Calendar (Umm al-Qura)", es419: "calendario hijri Umm al-Qura", zhHant: "伊斯蘭曆（烏姆庫拉）" },
            { calendar: "iso8601", en: "ISO-8601 Calendar", es419: "calendario ISO-8601", zhHant: "ISO 8601 國際曆法" },
            { calendar: "japanese", en: "Japanese Calendar", es419: "calendario japonés", zhHant: "日本曆" },
            { calendar: "persian", en: "Persian Calendar", es419: "calendario persa", zhHant: "波斯曆" },
            { calendar: "roc", en: "Minguo Calendar", es419: "calendario de la República de China", zhHant: "國曆" },
        ];

        const en = new Intl.DisplayNames("en", { type: "calendar" });
        const es419 = new Intl.DisplayNames("es-419", { type: "calendar" });
        const zhHant = new Intl.DisplayNames("zh-Hant", { type: "calendar" });

        data.forEach(d => {
            expect(en.of(d.calendar)).toBe(d.en);
            expect(es419.of(d.calendar)).toBe(d.es419);
            expect(zhHant.of(d.calendar)).toBe(d.zhHant);
        });
    });

    test("option type dateTimeField, style long", () => {
        // prettier-ignore
        const data = [
            { dateTimeField: "era", en: "era", es419: "era", zhHant: "年代" },
            { dateTimeField: "year", en: "year", es419: "año", zhHant: "年" },
            { dateTimeField: "quarter", en: "quarter", es419: "trimestre", zhHant: "季" },
            { dateTimeField: "month", en: "month", es419: "mes", zhHant: "月" },
            { dateTimeField: "weekOfYear", en: "week", es419: "semana", zhHant: "週" },
            { dateTimeField: "weekday", en: "day of the week", es419: "día de la semana", zhHant: "週天" },
            { dateTimeField: "day", en: "day", es419: "día", zhHant: "日" },
            { dateTimeField: "dayPeriod", en: "AM/PM", es419: "a.m./p.m.", zhHant: "上午/下午" },
            { dateTimeField: "hour", en: "hour", es419: "hora", zhHant: "小時" },
            { dateTimeField: "minute", en: "minute", es419: "minuto", zhHant: "分鐘" },
            { dateTimeField: "second", en: "second", es419: "segundo", zhHant: "秒" },
            { dateTimeField: "timeZoneName", en: "time zone", es419: "zona horaria", zhHant: "時區" },
        ];

        const en = new Intl.DisplayNames("en", { type: "dateTimeField", style: "long" });
        const es419 = new Intl.DisplayNames("es-419", { type: "dateTimeField", style: "long" });
        const zhHant = new Intl.DisplayNames("zh-Hant", { type: "dateTimeField", style: "long" });

        data.forEach(d => {
            expect(en.of(d.dateTimeField)).toBe(d.en);
            expect(es419.of(d.dateTimeField)).toBe(d.es419);
            expect(zhHant.of(d.dateTimeField)).toBe(d.zhHant);
        });
    });

    test("option type dateTimeField, style short", () => {
        // prettier-ignore
        const data = [
            { dateTimeField: "era", en: "era", es419: "era", zhHant: "年代" },
            { dateTimeField: "year", en: "yr.", es419: "a", zhHant: "年" },
            { dateTimeField: "quarter", en: "qtr.", es419: "trim.", zhHant: "季" },
            { dateTimeField: "month", en: "mo.", es419: "m", zhHant: "月" },
            { dateTimeField: "weekOfYear", en: "wk.", es419: "sem.", zhHant: "週" },
            { dateTimeField: "weekday", en: "day of wk.", es419: "día de sem.", zhHant: "週天" },
            { dateTimeField: "day", en: "day", es419: "d", zhHant: "日" },
            { dateTimeField: "dayPeriod", en: "AM/PM", es419: "a.m./p.m.", zhHant: "上午/下午" },
            { dateTimeField: "hour", en: "hr.", es419: "h", zhHant: "小時" },
            { dateTimeField: "minute", en: "min.", es419: "min", zhHant: "分鐘" },
            { dateTimeField: "second", en: "sec.", es419: "s", zhHant: "秒" },
            { dateTimeField: "timeZoneName", en: "zone", es419: "zona", zhHant: "時區" },
        ];

        const en = new Intl.DisplayNames("en", { type: "dateTimeField", style: "short" });
        const es419 = new Intl.DisplayNames("es-419", { type: "dateTimeField", style: "short" });
        const zhHant = new Intl.DisplayNames("zh-Hant", { type: "dateTimeField", style: "short" });

        data.forEach(d => {
            expect(en.of(d.dateTimeField)).toBe(d.en);
            expect(es419.of(d.dateTimeField)).toBe(d.es419);
            expect(zhHant.of(d.dateTimeField)).toBe(d.zhHant);
        });
    });

    test("option type dateTimeField, style narrow", () => {
        // prettier-ignore
        const data = [
            { dateTimeField: "era", en: "era", es419: "era", zhHant: "年代" },
            { dateTimeField: "year", en: "yr", es419: "a", zhHant: "年" },
            { dateTimeField: "quarter", en: "qtr", es419: "trim.", zhHant: "季" },
            { dateTimeField: "month", en: "mo", es419: "m", zhHant: "月" },
            { dateTimeField: "weekOfYear", en: "wk", es419: "sem.", zhHant: "週" },
            { dateTimeField: "weekday", en: "day of wk.", es419: "día de sem.", zhHant: "週天" },
            { dateTimeField: "day", en: "day", es419: "d", zhHant: "日" },
            { dateTimeField: "dayPeriod", en: "AM/PM", es419: "a.m./p.m.", zhHant: "上午/下午" },
            { dateTimeField: "hour", en: "hr", es419: "h", zhHant: "小時" },
            { dateTimeField: "minute", en: "min", es419: "min", zhHant: "分鐘" },
            { dateTimeField: "second", en: "sec", es419: "s", zhHant: "秒" },
            { dateTimeField: "timeZoneName", en: "zone", es419: "zona", zhHant: "時區" },
        ];

        const en = new Intl.DisplayNames("en", { type: "dateTimeField", style: "narrow" });
        const es419 = new Intl.DisplayNames("es-419", { type: "dateTimeField", style: "narrow" });
        const zhHant = new Intl.DisplayNames("zh-Hant", { type: "dateTimeField", style: "narrow" });

        data.forEach(d => {
            expect(en.of(d.dateTimeField)).toBe(d.en);
            expect(es419.of(d.dateTimeField)).toBe(d.es419);
            expect(zhHant.of(d.dateTimeField)).toBe(d.zhHant);
        });
    });
});
