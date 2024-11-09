describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.Instant();
        }).toThrowWithMessage(TypeError, "Temporal.Instant constructor must be called with 'new'");
    });

    test("argument must be coercible to bigint", () => {
        expect(() => {
            new Temporal.Instant(123);
        }).toThrowWithMessage(TypeError, "Cannot convert number to BigInt");
        expect(() => {
            new Temporal.Instant("foo");
        }).toThrowWithMessage(SyntaxError, "Invalid value for BigInt: foo");
    });
});

describe("normal behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant).toHaveLength(1);
    });

    test("basic functionality", () => {
        const instant = new Temporal.Instant(123n);
        expect(instant.epochNanoseconds).toBe(123n);
        expect(typeof instant).toBe("object");
        expect(instant).toBeInstanceOf(Temporal.Instant);
        expect(Object.getPrototypeOf(instant)).toBe(Temporal.Instant.prototype);
    });
});
