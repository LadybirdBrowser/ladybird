describe("correct behavior", () => {
    test("length", () => {
        expect(Number.prototype.valueOf).toHaveLength(0);
    });

    test("basic functionality", () => {
        expect(new Number(42).valueOf()).toBe(42);
        expect(Number.prototype.valueOf.call(42)).toBe(42);
    });
});

describe("errors", () => {
    test("must be called with numeric |this|", () => {
        [true, [], {}, Symbol("foo"), "bar", 1n].forEach(value => {
            expect(() => Number.prototype.valueOf.call(value)).toThrowWithMessage(
                TypeError,
                "Not an object of type Number"
            );
        });
    });
});
