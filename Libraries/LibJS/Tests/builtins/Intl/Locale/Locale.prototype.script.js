describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.script;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").script).toBeUndefined();
        expect(new Intl.Locale("en-Latn").script).toBe("Latn");
        expect(new Intl.Locale("en-GB").script).toBeUndefined();
        expect(new Intl.Locale("en", { script: "Latn" }).script).toBe("Latn");
        expect(new Intl.Locale("en", { region: "GB" }).script).toBeUndefined();

        expect(new Intl.Locale("en-u-ca-abc").script).toBeUndefined();
        expect(new Intl.Locale("en", { calendar: "abc" }).script).toBeUndefined();
        expect(new Intl.Locale("en-x-abcd").script).toBeUndefined();
    });
});
