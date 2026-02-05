describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.getNumberingSystems();
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    const testNumberingSystems = (locale, expected) => {
        const result = locale.getNumberingSystems();
        expect(Array.isArray(result)).toBeTrue();

        for (const entry of expected) {
            expect(result).toContain(entry);
        }
    };

    test("basic functionality", () => {
        testNumberingSystems(new Intl.Locale("en"), ["latn"]);
        testNumberingSystems(new Intl.Locale("ar"), ["arab", "latn"]);
    });

    test("extension keyword overrides default data", () => {
        testNumberingSystems(new Intl.Locale("en-u-nu-deva"), ["deva"]);
        testNumberingSystems(new Intl.Locale("en", { numberingSystem: "deva" }), ["deva"]);

        testNumberingSystems(new Intl.Locale("ar-u-nu-bali"), ["bali"]);
        testNumberingSystems(new Intl.Locale("ar", { numberingSystem: "bali" }), ["bali"]);

        // Invalid numberingSystems also take precedence.
        testNumberingSystems(new Intl.Locale("en-u-nu-ladybird"), ["ladybird"]);
        testNumberingSystems(new Intl.Locale("en", { numberingSystem: "ladybird" }), ["ladybird"]);
    });
});
