describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.subtract).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(1, 2, 3, 4, 5, 6);
        const duration = new Temporal.Duration(2021, 10, 1, 1, 0, 1, 2, 3, 4, 5);
        const result = plainTime.subtract(duration);
        expect(result.hour).toBe(1);
        expect(result.minute).toBe(1);
        expect(result.second).toBe(1);
        expect(result.millisecond).toBe(1);
        expect(result.microsecond).toBe(1);
        expect(result.nanosecond).toBe(1);
    });
});

describe("errors", () => {
    test("invalid duration-like", () => {
        const plainTime = new Temporal.PlainTime(1, 1, 1, 1, 1, 1);
        const invalidDuration = { foo: 1, bar: 2 };
        expect(() => {
            plainTime.subtract(invalidDuration);
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });
});
