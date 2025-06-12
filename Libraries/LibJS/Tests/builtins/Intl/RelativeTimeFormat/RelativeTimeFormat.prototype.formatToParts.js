describe("errors", () => {
    test("called on non-RelativeimeFormat object", () => {
        expect(() => {
            Intl.RelativeTimeFormat.prototype.formatToParts(1);
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.RelativeTimeFormat");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(Symbol.hasInstance, "year");
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(1n, "year");
        }).toThrowWithMessage(TypeError, "Cannot convert BigInt to number");
    });

    test("called with unit that cannot be converted to a number", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
    });

    test("called with non-finite value", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(Infinity, "year");
        }).toThrowWithMessage(RangeError, "Number must not be NaN or Infinity");

        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(-Infinity, "year");
        }).toThrowWithMessage(RangeError, "Number must not be NaN or Infinity");

        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(NaN, "year");
        }).toThrowWithMessage(RangeError, "Number must not be NaN or Infinity");
    });

    test("called with non-sanctioned unit", () => {
        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(1);
        }).toThrowWithMessage(RangeError, "Unit undefined is not a valid time unit");

        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(1, null);
        }).toThrowWithMessage(RangeError, "Unit null is not a valid time unit");

        expect(() => {
            new Intl.RelativeTimeFormat().formatToParts(1, "hello!");
        }).toThrowWithMessage(RangeError, "Unit hello! is not a valid time unit");
    });
});

const en = new Intl.RelativeTimeFormat("en", { style: "long", numeric: "always" });
const ar = new Intl.RelativeTimeFormat("ar", { style: "narrow", numeric: "auto" });

describe("correct behavior", () => {
    test("second", () => {
        expect(en.formatToParts(5, "second")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "second" },
            { type: "literal", value: " seconds" },
        ]);

        expect(ar.formatToParts(-1, "second")).toEqual([
            { type: "literal", value: "قبل ثانية واحدة" },
        ]);
    });

    test("minute", () => {
        expect(en.formatToParts(5, "minute")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "minute" },
            { type: "literal", value: " minutes" },
        ]);

        expect(ar.formatToParts(-1, "minute")).toEqual([
            { type: "literal", value: "قبل دقيقة واحدة" },
        ]);
    });

    test("hour", () => {
        expect(en.formatToParts(5, "hour")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "hour" },
            { type: "literal", value: " hours" },
        ]);

        expect(ar.formatToParts(-1, "hour")).toEqual([
            { type: "literal", value: "قبل ساعة واحدة" },
        ]);
    });

    test("day", () => {
        expect(en.formatToParts(5, "day")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "day" },
            { type: "literal", value: " days" },
        ]);

        expect(ar.formatToParts(-1, "day")).toEqual([{ type: "literal", value: "أمس" }]);
    });

    test("week", () => {
        expect(en.formatToParts(5, "week")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "week" },
            { type: "literal", value: " weeks" },
        ]);

        expect(ar.formatToParts(-1, "week")).toEqual([
            { type: "literal", value: "الأسبوع الماضي" },
        ]);
    });

    test("month", () => {
        expect(en.formatToParts(5, "month")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "month" },
            { type: "literal", value: " months" },
        ]);

        expect(ar.formatToParts(-1, "month")).toEqual([{ type: "literal", value: "الشهر الماضي" }]);
    });

    test("quarter", () => {
        expect(en.formatToParts(5, "quarter")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "quarter" },
            { type: "literal", value: " quarters" },
        ]);

        expect(ar.formatToParts(-1, "quarter")).toEqual([
            { type: "literal", value: "الربع الأخير" },
        ]);
    });

    test("year", () => {
        expect(en.formatToParts(5, "year")).toEqual([
            { type: "literal", value: "in " },
            { type: "integer", value: "5", unit: "year" },
            { type: "literal", value: " years" },
        ]);

        expect(ar.formatToParts(-1, "year")).toEqual([{ type: "literal", value: "السنة الماضية" }]);
    });
});
