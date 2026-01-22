describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.getHourCycles();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    const testHourCycles = (locale, expected) => {
        const result = locale.getHourCycles();
        expect(Array.isArray(result)).toBeTrue();

        for (const entry of expected) {
            expect(result).toContain(entry);
        }
    };

    test("basic functionality", () => {
        testHourCycles(new Intl.Locale("en"), ["h12"]);
        testHourCycles(new Intl.Locale("ha"), ["h23"]);
    });

    test("extension keyword overrides default data", () => {
        testHourCycles(new Intl.Locale("en-u-hc-h24"), ["h24"]);
        testHourCycles(new Intl.Locale("en", { collation: "h24" }), ["h24"]);

        testHourCycles(new Intl.Locale("ar-u-hc-h24"), ["h24"]);
        testHourCycles(new Intl.Locale("ar", { collation: "h24" }), ["h24"]);

        // Invalid hourCycles also take precedence when specified in the locale string. Unlike other
        // properties, Locale("en", { hourCycle: "ladybird" }) will explicitly throw.
        testHourCycles(new Intl.Locale("en-u-hc-ladybird"), ["ladybird"]);
    });
});
