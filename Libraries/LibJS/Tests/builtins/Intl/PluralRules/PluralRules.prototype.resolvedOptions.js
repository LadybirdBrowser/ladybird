describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Intl.PluralRules.prototype.resolvedOptions).toHaveLength(0);
    });

    test("locale only contains relevant extension keys", () => {
        const en1 = new Intl.PluralRules("en-u-ca-islamicc");
        expect(en1.resolvedOptions().locale).toBe("en");

        const en2 = new Intl.PluralRules("en-u-nu-latn");
        expect(en2.resolvedOptions().locale).toBe("en");

        const en3 = new Intl.PluralRules("en-u-ca-islamicc-nu-latn");
        expect(en3.resolvedOptions().locale).toBe("en");
    });

    test("type", () => {
        const en1 = new Intl.PluralRules("en");
        expect(en1.resolvedOptions().type).toBe("cardinal");

        ["cardinal", "ordinal"].forEach(type => {
            const en2 = new Intl.PluralRules("en", { type: type });
            expect(en2.resolvedOptions().type).toBe(type);
        });
    });

    test("min integer digits", () => {
        const en1 = new Intl.PluralRules("en");
        expect(en1.resolvedOptions().minimumIntegerDigits).toBe(1);

        const en2 = new Intl.PluralRules("en", { minimumIntegerDigits: 5 });
        expect(en2.resolvedOptions().minimumIntegerDigits).toBe(5);
    });

    test("min/max fraction digits", () => {
        const en1 = new Intl.PluralRules("en", { minimumFractionDigits: 5 });
        expect(en1.resolvedOptions().minimumFractionDigits).toBe(5);
        expect(en1.resolvedOptions().maximumFractionDigits).toBe(5);
        expect(en1.resolvedOptions().minimumSignificantDigits).toBeUndefined();
        expect(en1.resolvedOptions().maximumSignificantDigits).toBeUndefined();

        const en2 = new Intl.PluralRules("en", { maximumFractionDigits: 5 });
        expect(en2.resolvedOptions().minimumFractionDigits).toBe(0);
        expect(en2.resolvedOptions().maximumFractionDigits).toBe(5);
        expect(en2.resolvedOptions().minimumSignificantDigits).toBeUndefined();
        expect(en2.resolvedOptions().maximumSignificantDigits).toBeUndefined();

        const en3 = new Intl.PluralRules("en", {
            minimumFractionDigits: 5,
            maximumFractionDigits: 10,
        });
        expect(en3.resolvedOptions().minimumFractionDigits).toBe(5);
        expect(en3.resolvedOptions().maximumFractionDigits).toBe(10);
        expect(en3.resolvedOptions().minimumSignificantDigits).toBeUndefined();
        expect(en3.resolvedOptions().maximumSignificantDigits).toBeUndefined();
    });

    test("min/max significant digits", () => {
        const en1 = new Intl.PluralRules("en", { minimumSignificantDigits: 5 });
        expect(en1.resolvedOptions().minimumFractionDigits).toBeUndefined();
        expect(en1.resolvedOptions().maximumFractionDigits).toBeUndefined();
        expect(en1.resolvedOptions().minimumSignificantDigits).toBe(5);
        expect(en1.resolvedOptions().maximumSignificantDigits).toBe(21);

        const en2 = new Intl.PluralRules("en", { maximumSignificantDigits: 5 });
        expect(en2.resolvedOptions().minimumFractionDigits).toBeUndefined();
        expect(en2.resolvedOptions().maximumFractionDigits).toBeUndefined();
        expect(en2.resolvedOptions().minimumSignificantDigits).toBe(1);
        expect(en2.resolvedOptions().maximumSignificantDigits).toBe(5);

        const en3 = new Intl.PluralRules("en", {
            minimumSignificantDigits: 5,
            maximumSignificantDigits: 10,
        });
        expect(en3.resolvedOptions().minimumFractionDigits).toBeUndefined();
        expect(en3.resolvedOptions().maximumFractionDigits).toBeUndefined();
        expect(en3.resolvedOptions().minimumSignificantDigits).toBe(5);
        expect(en3.resolvedOptions().maximumSignificantDigits).toBe(10);
    });

    test("plural categories", () => {
        // The spec doesn't dictate an order of elements, and the generated CLDR data is ordered by
        // hash map iteration. Instead of using toEqual(), just make sure all elements are the same.
        const contains = (actual, expected) => {
            if (actual.length !== expected.length) return false;
            return expected.every(e => actual.includes(e));
        };

        const enCardinal = new Intl.PluralRules("en", { type: "cardinal" }).resolvedOptions();
        expect(enCardinal.pluralCategories).toBeDefined();
        expect(contains(enCardinal.pluralCategories, ["other", "one"])).toBeTrue();

        const enOrdinal = new Intl.PluralRules("en", { type: "ordinal" }).resolvedOptions();
        expect(enOrdinal.pluralCategories).toBeDefined();
        expect(contains(enOrdinal.pluralCategories, ["other", "one", "two", "few"])).toBeTrue();

        const gaCardinal = new Intl.PluralRules("ga", { type: "cardinal" }).resolvedOptions();
        expect(gaCardinal.pluralCategories).toBeDefined();
        expect(
            contains(gaCardinal.pluralCategories, ["other", "one", "two", "few", "many"])
        ).toBeTrue();

        const gaOrdinal = new Intl.PluralRules("ga", { type: "ordinal" }).resolvedOptions();
        expect(gaOrdinal.pluralCategories).toBeDefined();
        expect(contains(gaOrdinal.pluralCategories, ["other", "one"])).toBeTrue();
    });

    test("rounding priority", () => {
        const en1 = new Intl.PluralRules("en");
        expect(en1.resolvedOptions().roundingPriority).toBe("auto");

        ["auto", "morePrecision", "lessPrecision"].forEach(roundingPriority => {
            const en2 = new Intl.PluralRules("en", { roundingPriority: roundingPriority });
            expect(en2.resolvedOptions().roundingPriority).toBe(roundingPriority);
        });
    });

    test("rounding mode", () => {
        const en1 = new Intl.PluralRules("en");
        expect(en1.resolvedOptions().roundingMode).toBe("halfExpand");

        [
            "ceil",
            "floor",
            "expand",
            "trunc",
            "halfCeil",
            "halfFloor",
            "halfExpand",
            "halfTrunc",
            "halfEven",
        ].forEach(roundingMode => {
            const en2 = new Intl.PluralRules("en", { roundingMode: roundingMode });
            expect(en2.resolvedOptions().roundingMode).toBe(roundingMode);
        });
    });

    test("rounding increment", () => {
        const en1 = new Intl.PluralRules("en");
        expect(en1.resolvedOptions().roundingIncrement).toBe(1);

        [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000].forEach(
            roundingIncrement => {
                const en2 = new Intl.PluralRules("en", { roundingIncrement: roundingIncrement });
                expect(en2.resolvedOptions().roundingIncrement).toBe(roundingIncrement);
            }
        );
    });

    test("trailing zero display", () => {
        const en1 = new Intl.PluralRules("en");
        expect(en1.resolvedOptions().trailingZeroDisplay).toBe("auto");

        ["auto", "stripIfInteger"].forEach(trailingZeroDisplay => {
            const en2 = new Intl.PluralRules("en", { trailingZeroDisplay: trailingZeroDisplay });
            expect(en2.resolvedOptions().trailingZeroDisplay).toBe(trailingZeroDisplay);
        });
    });
});
