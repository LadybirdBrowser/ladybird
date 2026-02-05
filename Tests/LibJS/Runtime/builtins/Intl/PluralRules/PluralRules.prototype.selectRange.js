describe("errors", () => {
    test("called on non-PluralRules object", () => {
        expect(() => {
            Intl.PluralRules.prototype.selectRange();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.PluralRules");
    });

    test("called without enough values", () => {
        expect(() => {
            new Intl.PluralRules().selectRange();
        }).toThrowWithMessage(TypeError, "start is undefined");

        expect(() => {
            new Intl.PluralRules().selectRange(1);
        }).toThrowWithMessage(TypeError, "end is undefined");
    });

    test("called with values that cannot be converted to numbers", () => {
        expect(() => {
            new Intl.PluralRules().selectRange(Symbol.hasInstance, 1);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");

        expect(() => {
            new Intl.PluralRules().selectRange(1, Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });

    test("called with invalid numbers", () => {
        expect(() => {
            new Intl.PluralRules().selectRange(NaN, 1);
        }).toThrowWithMessage(RangeError, "start must not be NaN");

        expect(() => {
            new Intl.PluralRules().selectRange(1, NaN);
        }).toThrowWithMessage(RangeError, "end must not be NaN");
    });
});

describe("correct behavior", () => {
    test("basic functionality", () => {
        const en = new Intl.PluralRules("en");
        expect(en.selectRange(1, 1)).toBe("one"); // one + one = one
        expect(en.selectRange(1, 2)).toBe("other"); // one + other = other
        expect(en.selectRange(0, 1)).toBe("other"); // other + one = other
        expect(en.selectRange(2, 3)).toBe("other"); // other + other = other

        const pl = new Intl.PluralRules("pl");
        expect(pl.selectRange(1, 1)).toBe("one"); // one + one = one
        expect(pl.selectRange(1, 2)).toBe("few"); // one + few = few
        expect(pl.selectRange(1, 5)).toBe("many"); // one + many = many
        expect(pl.selectRange(1, 3.14)).toBe("other"); // one + other = other
        expect(pl.selectRange(2, 2)).toBe("few"); // few + few = few
        expect(pl.selectRange(2, 5)).toBe("many"); // few + many = many
        expect(pl.selectRange(2, 3.14)).toBe("other"); // few + other = other
        expect(pl.selectRange(0, 1)).toBe("one"); // many + one = one
        expect(pl.selectRange(0, 2)).toBe("few"); // many + few = few
        expect(pl.selectRange(0, 5)).toBe("many"); // many + many = many
        expect(pl.selectRange(0, 3.14)).toBe("other"); // many + other = other
        expect(pl.selectRange(0.14, 1)).toBe("one"); // other + one = one
        expect(pl.selectRange(0.14, 2)).toBe("few"); // other + few = few
        expect(pl.selectRange(0.14, 5)).toBe("many"); // other + many = many
        expect(pl.selectRange(0.14, 3.14)).toBe("other"); // other + other = other
    });

    test("numbers in reverse order", () => {
        const en = new Intl.PluralRules("en");
        expect(en.selectRange(1, -Infinity)).toBe("other");
        expect(en.selectRange(Infinity, -Infinity)).toBe("other");
        expect(en.selectRange(-0, -Infinity)).toBe("other");

        const ja = new Intl.PluralRules("ja");
        expect(ja.selectRange(1, -Infinity)).toBe("other");
        expect(ja.selectRange(Infinity, -Infinity)).toBe("other");
        expect(ja.selectRange(-0, -Infinity)).toBe("other");
    });
});
