describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.baseName;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").baseName).toBe("en");
        expect(new Intl.Locale("en-Latn").baseName).toBe("en-Latn");
        expect(new Intl.Locale("en-GB").baseName).toBe("en-GB");
        expect(new Intl.Locale("en", { script: "Latn" }).baseName).toBe("en-Latn");
        expect(new Intl.Locale("en", { region: "GB" }).baseName).toBe("en-GB");

        expect(new Intl.Locale("en-u-ca-abc").baseName).toBe("en");
        expect(new Intl.Locale("en", { calendar: "abc" }).baseName).toBe("en");
        expect(new Intl.Locale("en-x-abcd").baseName).toBe("en");
    });
});
