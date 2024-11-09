describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.numeric;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").numeric).toBeFalse();
        expect(new Intl.Locale("en-u-kn-true").numeric).toBeTrue();
        expect(new Intl.Locale("en", { numeric: false }).numeric).toBeFalse();
        expect(new Intl.Locale("en-u-kn-false", { numeric: true }).numeric).toBeTrue();
    });
});
