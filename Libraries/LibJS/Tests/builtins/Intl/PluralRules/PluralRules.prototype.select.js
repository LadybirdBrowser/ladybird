describe("errors", () => {
    test("called on non-PluralRules object", () => {
        expect(() => {
            Intl.PluralRules.prototype.select();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.PluralRules");
    });

    test("called with value that cannot be converted to a number", () => {
        expect(() => {
            new Intl.PluralRules().select(Symbol.hasInstance);
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});

describe("non-finite values", () => {
    test("NaN", () => {
        expect(new Intl.PluralRules("en").select(NaN)).toBe("other");
        expect(new Intl.PluralRules("ar").select(NaN)).toBe("other");
        expect(new Intl.PluralRules("pl").select(NaN)).toBe("other");
    });

    test("Infinity", () => {
        expect(new Intl.PluralRules("en").select(Infinity)).toBe("other");
        expect(new Intl.PluralRules("ar").select(Infinity)).toBe("other");
        expect(new Intl.PluralRules("pl").select(Infinity)).toBe("other");
    });

    test("-Infinity", () => {
        expect(new Intl.PluralRules("en").select(-Infinity)).toBe("other");
        expect(new Intl.PluralRules("ar").select(-Infinity)).toBe("other");
        expect(new Intl.PluralRules("pl").select(-Infinity)).toBe("other");
    });
});

describe("correct behavior", () => {
    test("cardinal", () => {
        const en = new Intl.PluralRules("en", { type: "cardinal" });
        expect(en.select(0)).toBe("other");
        expect(en.select(1)).toBe("one");
        expect(en.select(2)).toBe("other");
        expect(en.select(3)).toBe("other");

        // In "he":
        // "one" is specified to be the integer 1, and non-integers whose integer part is 0.
        // "two" is specified to be the integer 2.
        const he = new Intl.PluralRules("he", { type: "cardinal" });
        expect(he.select(0)).toBe("other");
        expect(he.select(1)).toBe("one");
        expect(he.select(0.1)).toBe("one");
        expect(he.select(0.2)).toBe("one");
        expect(he.select(0.8)).toBe("one");
        expect(he.select(0.9)).toBe("one");
        expect(he.select(2)).toBe("two");
        expect(he.select(10)).toBe("other");
        expect(he.select(19)).toBe("other");
        expect(he.select(20)).toBe("other");
        expect(he.select(21)).toBe("other");
        expect(he.select(29)).toBe("other");
        expect(he.select(30)).toBe("other");
        expect(he.select(31)).toBe("other");

        // In "pl":
        // "few" is specified to be integers such that (i % 10 == 2..4 && i % 100 != 12..14).
        // "many" is specified to be all other integers != 1.
        // "other" is specified to be non-integers.
        const pl = new Intl.PluralRules("pl", { type: "cardinal" });
        expect(pl.select(0)).toBe("many");
        expect(pl.select(1)).toBe("one");
        expect(pl.select(2)).toBe("few");
        expect(pl.select(3)).toBe("few");
        expect(pl.select(4)).toBe("few");
        expect(pl.select(5)).toBe("many");
        expect(pl.select(12)).toBe("many");
        expect(pl.select(13)).toBe("many");
        expect(pl.select(14)).toBe("many");
        expect(pl.select(21)).toBe("many");
        expect(pl.select(22)).toBe("few");
        expect(pl.select(23)).toBe("few");
        expect(pl.select(24)).toBe("few");
        expect(pl.select(25)).toBe("many");
        expect(pl.select(3.14)).toBe("other");

        // In "am":
        // "one" is specified to be the integers 0 and 1, and non-integers whose integer part is 0.
        const am = new Intl.PluralRules("am", { type: "cardinal" });
        expect(am.select(0)).toBe("one");
        expect(am.select(0.1)).toBe("one");
        expect(am.select(0.2)).toBe("one");
        expect(am.select(0.8)).toBe("one");
        expect(am.select(0.9)).toBe("one");
        expect(am.select(1)).toBe("one");
        expect(am.select(1.1)).toBe("other");
        expect(am.select(1.9)).toBe("other");
        expect(am.select(2)).toBe("other");
        expect(am.select(3)).toBe("other");
    });

    test("ordinal", () => {
        // In "en":
        // "one" is specified to be integers such that (i % 10 == 1), excluding 11.
        // "two" is specified to be integers such that (i % 10 == 2), excluding 12.
        // "few" is specified to be integers such that (i % 10 == 3), excluding 13.
        const en = new Intl.PluralRules("en", { type: "ordinal" });
        expect(en.select(0)).toBe("other");
        expect(en.select(1)).toBe("one");
        expect(en.select(2)).toBe("two");
        expect(en.select(3)).toBe("few");
        expect(en.select(4)).toBe("other");
        expect(en.select(10)).toBe("other");
        expect(en.select(11)).toBe("other");
        expect(en.select(12)).toBe("other");
        expect(en.select(13)).toBe("other");
        expect(en.select(14)).toBe("other");
        expect(en.select(20)).toBe("other");
        expect(en.select(21)).toBe("one");
        expect(en.select(22)).toBe("two");
        expect(en.select(23)).toBe("few");
        expect(en.select(24)).toBe("other");

        // In "mk":
        // "one" is specified to be integers such that (i % 10 == 1 && i % 100 != 11).
        // "two" is specified to be integers such that (i % 10 == 2 && i % 100 != 12).
        // "many" is specified to be integers such that (i % 10 == 7,8 && i % 100 != 17,18).
        const mk = new Intl.PluralRules("mk", { type: "ordinal" });
        expect(mk.select(0)).toBe("other");
        expect(mk.select(1)).toBe("one");
        expect(mk.select(2)).toBe("two");
        expect(mk.select(3)).toBe("other");
        expect(mk.select(6)).toBe("other");
        expect(mk.select(7)).toBe("many");
        expect(mk.select(8)).toBe("many");
        expect(mk.select(9)).toBe("other");
        expect(mk.select(11)).toBe("other");
        expect(mk.select(12)).toBe("other");
        expect(mk.select(17)).toBe("other");
        expect(mk.select(18)).toBe("other");
        expect(mk.select(21)).toBe("one");
        expect(mk.select(22)).toBe("two");
        expect(mk.select(27)).toBe("many");
        expect(mk.select(28)).toBe("many");
    });
});
