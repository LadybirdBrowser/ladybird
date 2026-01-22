describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.region;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").region).toBeUndefined();
        expect(new Intl.Locale("en-Latn").region).toBeUndefined();
        expect(new Intl.Locale("en-GB").region).toBe("GB");
        expect(new Intl.Locale("en", { script: "Latn" }).region).toBeUndefined();
        expect(new Intl.Locale("en", { region: "GB" }).region).toBe("GB");

        expect(new Intl.Locale("en-u-ca-abc").region).toBeUndefined();
        expect(new Intl.Locale("en", { calendar: "abc" }).region).toBeUndefined();
        expect(new Intl.Locale("en-x-abcd").region).toBeUndefined();
    });
});
