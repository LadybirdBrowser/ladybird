describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.getCollations();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    const testCollations = (locale, expected) => {
        const result = locale.getCollations();
        expect(Array.isArray(result)).toBeTrue();

        for (const entry of expected) {
            expect(result).toContain(entry);
        }
    };

    test("basic functionality", () => {
        testCollations(new Intl.Locale("en"), ["emoji", "eor"]);
        testCollations(new Intl.Locale("ar"), ["compat", "emoji", "eor"]);
    });

    test("default collation is not included", () => {
        expect(new Intl.Locale("en").getCollations()).not.toContain("default");
        expect(new Intl.Locale("ar").getCollations()).not.toContain("default");
    });

    test("collations are sorted", () => {
        const collations = new Intl.Locale("de").getCollations();
        expect(collations).toEqual([...collations].sort());
    });

    test("unavailable locales use the root collations", () => {
        expect(new Intl.Locale("und").getCollations()).toEqual(["emoji", "eor"]);
        expect(new Intl.Locale("qfz").getCollations()).toEqual(["emoji", "eor"]);
    });

    test("extension keyword overrides default data", () => {
        testCollations(new Intl.Locale("en-u-co-compat"), ["compat"]);
        testCollations(new Intl.Locale("en", { collation: "compat" }), ["compat"]);

        testCollations(new Intl.Locale("ar-u-co-reformed"), ["reformed"]);
        testCollations(new Intl.Locale("ar", { collation: "reformed" }), ["reformed"]);

        // Invalid getCollations() also take precedence.
        testCollations(new Intl.Locale("en-u-co-ladybird"), ["ladybird"]);
        testCollations(new Intl.Locale("en", { collation: "ladybird" }), ["ladybird"]);
    });
});
