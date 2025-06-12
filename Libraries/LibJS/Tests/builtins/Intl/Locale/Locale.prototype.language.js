describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.language;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").language).toBe("en");
        expect(new Intl.Locale("en-Latn").language).toBe("en");
        expect(new Intl.Locale("en-GB").language).toBe("en");
        expect(new Intl.Locale("en", { script: "Latn" }).language).toBe("en");
        expect(new Intl.Locale("en", { region: "GB" }).language).toBe("en");

        expect(new Intl.Locale("en-u-ca-abc").language).toBe("en");
        expect(new Intl.Locale("en", { calendar: "abc" }).language).toBe("en");
        expect(new Intl.Locale("en-x-abcd").language).toBe("en");
    });
});
