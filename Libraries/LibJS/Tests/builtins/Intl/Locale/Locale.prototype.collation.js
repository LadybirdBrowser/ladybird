describe("errors", () => {
    test("called on non-Locale object", () => {
        expect(() => {
            Intl.Locale.prototype.collation;
        }).toThrowWithMessage(TypeError, "Not an object of type Intl.Locale");
    });
});

describe("normal behavior", () => {
    test("basic functionality", () => {
        expect(new Intl.Locale("en").collation).toBeUndefined();
        expect(new Intl.Locale("en-u-co-abc").collation).toBe("abc");
        expect(new Intl.Locale("en", { collation: "abc" }).collation).toBe("abc");
        expect(new Intl.Locale("en-u-co-abc", { collation: "def" }).collation).toBe("def");
    });
});
