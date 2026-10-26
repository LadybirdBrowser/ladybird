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

    test("option values are canonicalized without regard to case", () => {
        const values = [
            ["PHONEBK", "phonebk"],
            ["UNKNOWN", "unknown"],
        ];

        for (const tag of ["en", "en-u-co-abc"]) {
            for (const [value, expected] of values) {
                const locale = new Intl.Locale(tag, { collation: value });
                expect(locale.collation).toBe(expected);
                expect(locale.toString()).toBe(`en-u-co-${expected}`);
            }
        }
    });
});
