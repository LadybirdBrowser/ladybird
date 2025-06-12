describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.numberingSystem;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").numberingSystem).toBeUndefined();
        expect(new Intl.Locale("en-u-nu-abc").numberingSystem).toBe("abc");
        expect(new Intl.Locale("en", { numberingSystem: "abc" }).numberingSystem).toBe("abc");
        expect(new Intl.Locale("en-u-nu-abc", { numberingSystem: "def" }).numberingSystem).toBe(
            "def"
        );
    });
});
