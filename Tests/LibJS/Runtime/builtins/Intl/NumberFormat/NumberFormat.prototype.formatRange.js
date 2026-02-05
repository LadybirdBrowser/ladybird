describe("errors", () => {
    test("called on non-NumberFormat object", () => {
        expect(() => {
            Intl.NumberFormat.prototype.formatRange();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.NumberFormat");
    });

    test("called without enough values", () => {
        expect(() => {
            new Intl.NumberFormat().formatRange();
        }).toThrowWithMessage(TypeError, "start is undefined");

        expect(() => {
            new Intl.NumberFormat().formatRange(1);
        }).toThrowWithMessage(TypeError, "end is undefined");
    });

    test("called with values that cannot be converted to numbers", () => {
        expect(() => {
            new Intl.NumberFormat().formatRange(Symbol.hasInstance, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Intl.NumberFormat().formatRange(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });

    test("called with invalid numbers", () => {
        expect(() => {
            new Intl.NumberFormat().formatRange(NaN, 1);
        }).toThrowWithMessage(RangeError, "start must not be NaN");

        expect(() => {
            new Intl.NumberFormat().formatRange(1, NaN);
        }).toThrowWithMessage(RangeError, "end must not be NaN");
    });
});

describe("correct behavior", () => {
    test("basic functionality", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.formatRange(100, 101)).toBe("100–101");
        expect(en1.formatRange(3.14, 6.28)).toBe("3.14–6.28");
        expect(en1.formatRange(-0, 1)).toBe("-0 – 1");

        const ja1 = new Intl.NumberFormat("ja");
        expect(ja1.formatRange(100, 101)).toBe("100～101");
        expect(ja1.formatRange(3.14, 6.28)).toBe("3.14～6.28");
        expect(ja1.formatRange(-0, 1)).toBe("-0 ～ 1");
    });

    test("approximately formatting", () => {
        const en1 = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });
        expect(en1.formatRange(2.9, 3.1)).toBe("~3");
        expect(en1.formatRange(-3.1, -2.9)).toBe("~-3");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            maximumFractionDigits: 0,
        });
        expect(en2.formatRange(2.9, 3.1)).toBe("~$3");
        expect(en2.formatRange(-3.1, -2.9)).toBe("~-$3");

        const ja1 = new Intl.NumberFormat("ja", { maximumFractionDigits: 0 });
        expect(ja1.formatRange(2.9, 3.1)).toBe("約3");
        expect(ja1.formatRange(-3.1, -2.9)).toBe("約-3");

        const ja2 = new Intl.NumberFormat("ja", {
            style: "currency",
            currency: "JPY",
            maximumFractionDigits: 0,
        });
        expect(ja2.formatRange(2.9, 3.1)).toBe("約￥3");
        expect(ja2.formatRange(-3.1, -2.9)).toBe("約-￥3");
    });

    test("range pattern spacing", () => {
        const en1 = new Intl.NumberFormat("en");
        expect(en1.formatRange(3, 5)).toBe("3–5");
        expect(en1.formatRange(-1, -0)).toBe("-1 – -0");
        expect(en1.formatRange(0, Infinity)).toBe("0–∞");
        expect(en1.formatRange(-Infinity, 0)).toBe("-∞ – 0");

        const en2 = new Intl.NumberFormat("en", {
            style: "currency",
            currency: "USD",
            maximumFractionDigits: 0,
        });
        expect(en2.formatRange(3, 5)).toBe("$3 – $5");

        const ja1 = new Intl.NumberFormat("ja");
        expect(ja1.formatRange(3, 5)).toBe("3～5");
        expect(ja1.formatRange(-1, -0)).toBe("-1 ～ -0");
        expect(ja1.formatRange(0, Infinity)).toBe("0～∞");
        expect(ja1.formatRange(-Infinity, 0)).toBe("-∞ ～ 0");

        const ja2 = new Intl.NumberFormat("ja", {
            style: "currency",
            currency: "JPY",
            maximumFractionDigits: 0,
        });
        expect(ja2.formatRange(3, 5)).toBe("￥3 ～ ￥5");
    });

    test("numbers in reverse order", () => {
        const en = new Intl.NumberFormat("en");
        expect(en.formatRange(1, 0)).toBe("1–0");
        expect(en.formatRange(1, -Infinity)).toBe("1–-∞");
        expect(en.formatRange(1, -0)).toBe("1–-0");
        expect(en.formatRange(Infinity, 0)).toBe("∞–0");
        expect(en.formatRange(Infinity, -Infinity)).toBe("∞–-∞");
        expect(en.formatRange(Infinity, -0)).toBe("∞–-0");
        expect(en.formatRange(-0, -1)).toBe("-0 – -1");
        expect(en.formatRange(-0, -Infinity)).toBe("-0 – -∞");

        const ja = new Intl.NumberFormat("ja");
        expect(ja.formatRange(1, 0)).toBe("1～0");
        expect(ja.formatRange(1, -Infinity)).toBe("1～-∞");
        expect(ja.formatRange(1, -0)).toBe("1～-0");
        expect(ja.formatRange(Infinity, 0)).toBe("∞～0");
        expect(ja.formatRange(Infinity, -Infinity)).toBe("∞～-∞");
        expect(ja.formatRange(Infinity, -0)).toBe("∞～-0");
        expect(ja.formatRange(-0, -1)).toBe("-0 ～ -1");
        expect(ja.formatRange(-0, -Infinity)).toBe("-0 ～ -∞");
    });
});
