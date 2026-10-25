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
        expect(new Intl.Locale("en").getNumberingSystems()).toEqual(["latn"]);
        expect(new Intl.Locale("ar-EG").getNumberingSystems()).toEqual(["arab"]);
    });

    test("unavailable locales use latn", () => {
        expect(new Intl.Locale("und").getNumberingSystems()).toEqual(["latn"]);
        expect(new Intl.Locale("qfz").getNumberingSystems()).toEqual(["latn"]);
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
