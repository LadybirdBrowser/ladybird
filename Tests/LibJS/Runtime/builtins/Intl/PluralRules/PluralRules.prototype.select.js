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
    const testPluralRules = (pluralRules, number, expected) => {
        expect(pluralRules.select(number)).toBe(expected);

        if (Number.isInteger(number)) {
            expect(pluralRules.select(BigInt(number))).toBe(expected);
        }
    };

    test("cardinal", () => {
        const en = new Intl.PluralRules("en", { type: "cardinal" });
        testPluralRules(en, 0, "other");
        testPluralRules(en, 1, "one");
        testPluralRules(en, 2, "other");
        testPluralRules(en, 3, "other");

        // In "he":
        // "one" is specified to be the integer 1, and non-integers whose integer part is 0.
        // "two" is specified to be the integer 2.
        const he = new Intl.PluralRules("he", { type: "cardinal" });
        testPluralRules(he, 0, "other");
        testPluralRules(he, 1, "one");
        testPluralRules(he, 0.1, "one");
        testPluralRules(he, 0.2, "one");
        testPluralRules(he, 0.8, "one");
        testPluralRules(he, 0.9, "one");
        testPluralRules(he, 2, "two");
        testPluralRules(he, 10, "other");
        testPluralRules(he, 19, "other");
        testPluralRules(he, 20, "other");
        testPluralRules(he, 21, "other");
        testPluralRules(he, 29, "other");
        testPluralRules(he, 30, "other");
        testPluralRules(he, 31, "other");

        // In "pl":
        // "few" is specified to be integers such that (i % 10 == 2..4 && i % 100 != 12..14).
        // "many" is specified to be all other integers != 1.
        // "other" is specified to be non-integers.
        const pl = new Intl.PluralRules("pl", { type: "cardinal" });
        testPluralRules(pl, 0, "many");
        testPluralRules(pl, 1, "one");
        testPluralRules(pl, 2, "few");
        testPluralRules(pl, 3, "few");
        testPluralRules(pl, 4, "few");
        testPluralRules(pl, 5, "many");
        testPluralRules(pl, 12, "many");
        testPluralRules(pl, 13, "many");
        testPluralRules(pl, 14, "many");
        testPluralRules(pl, 21, "many");
        testPluralRules(pl, 22, "few");
        testPluralRules(pl, 23, "few");
        testPluralRules(pl, 24, "few");
        testPluralRules(pl, 25, "many");
        testPluralRules(pl, 3.14, "other");

        // In "am":
        // "one" is specified to be the integers 0 and 1, and non-integers whose integer part is 0.
        const am = new Intl.PluralRules("am", { type: "cardinal" });
        testPluralRules(am, 0, "one");
        testPluralRules(am, 0.1, "one");
        testPluralRules(am, 0.2, "one");
        testPluralRules(am, 0.8, "one");
        testPluralRules(am, 0.9, "one");
        testPluralRules(am, 1, "one");
        testPluralRules(am, 1.1, "other");
        testPluralRules(am, 1.9, "other");
        testPluralRules(am, 2, "other");
        testPluralRules(am, 3, "other");
    });

    test("ordinal", () => {
        // In "en":
        // "one" is specified to be integers such that (i % 10 == 1), excluding 11.
        // "two" is specified to be integers such that (i % 10 == 2), excluding 12.
        // "few" is specified to be integers such that (i % 10 == 3), excluding 13.
        const en = new Intl.PluralRules("en", { type: "ordinal" });
        testPluralRules(en, 0, "other");
        testPluralRules(en, 1, "one");
        testPluralRules(en, 2, "two");
        testPluralRules(en, 3, "few");
        testPluralRules(en, 4, "other");
        testPluralRules(en, 10, "other");
        testPluralRules(en, 11, "other");
        testPluralRules(en, 12, "other");
        testPluralRules(en, 13, "other");
        testPluralRules(en, 14, "other");
        testPluralRules(en, 20, "other");
        testPluralRules(en, 21, "one");
        testPluralRules(en, 22, "two");
        testPluralRules(en, 23, "few");
        testPluralRules(en, 24, "other");

        // In "mk":
        // "one" is specified to be integers such that (i % 10 == 1 && i % 100 != 11).
        // "two" is specified to be integers such that (i % 10 == 2 && i % 100 != 12).
        // "many" is specified to be integers such that (i % 10 == 7,8 && i % 100 != 17,18).
        const mk = new Intl.PluralRules("mk", { type: "ordinal" });
        testPluralRules(mk, 0, "other");
        testPluralRules(mk, 1, "one");
        testPluralRules(mk, 2, "two");
        testPluralRules(mk, 3, "other");
        testPluralRules(mk, 6, "other");
        testPluralRules(mk, 7, "many");
        testPluralRules(mk, 8, "many");
        testPluralRules(mk, 9, "other");
        testPluralRules(mk, 11, "other");
        testPluralRules(mk, 12, "other");
        testPluralRules(mk, 17, "other");
        testPluralRules(mk, 18, "other");
        testPluralRules(mk, 21, "one");
        testPluralRules(mk, 22, "two");
        testPluralRules(mk, 27, "many");
        testPluralRules(mk, 28, "many");
    });

    test("notation", () => {
        const standard = new Intl.PluralRules("fr", { notation: "standard" });
        const engineering = new Intl.PluralRules("fr", { notation: "engineering" });
        const scientific = new Intl.PluralRules("fr", { notation: "scientific" });
        const compact = new Intl.PluralRules("fr", { notation: "compact" });

        // prettier-ignore
        const data = [
            { value: 1e6, standard: "many", engineering: "many", scientific: "many", compact: "many" },
            { value: 1.5e6, standard: "other", engineering: "many", scientific: "many", compact: "many" },
            { value: 1e-6, standard: "one", engineering: "many", scientific: "many", compact: "one" },
        ];

        data.forEach(d => {
            testPluralRules(standard, d.value, d.standard);
            testPluralRules(engineering, d.value, d.engineering);
            testPluralRules(scientific, d.value, d.scientific);
            testPluralRules(compact, d.value, d.compact);
        });
    });
});
