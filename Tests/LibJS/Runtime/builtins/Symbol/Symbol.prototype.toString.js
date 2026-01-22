describe("correct behavior", () => {
    test("basic functionality", () => {
        const s1 = Symbol("baz");
        const s2 = Symbol.for("qux");

        // Explicit conversions to string are fine, but implicit toString via concatenation throws.
        expect(s1.toString()).toBe("Symbol(baz)");
        expect(String(s1)).toBe("Symbol(baz)");
        expect(s2.toString()).toBe("Symbol(qux)");
    });
});

describe("errors", () => {
    test("convert to string", () => {
        expect(() => {
            Symbol() + "";
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to string");
    });

    test("convert to number", () => {
        expect(() => {
            Symbol() + 1;
        }).toThrowWithMessage(TypeError, "Cannot convert symbol to number");
    });
});
