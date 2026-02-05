describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.add).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(1, 1, 1, 1, 1, 1);
        const duration = new Temporal.Duration(2021, 10, 1, 1, 0, 1, 2, 3, 4, 5);
        const result = plainTime.add(duration);
        expect(result.hour).toBe(1);
        expect(result.minute).toBe(2);
        expect(result.second).toBe(3);
        expect(result.millisecond).toBe(4);
        expect(result.microsecond).toBe(5);
        expect(result.nanosecond).toBe(6);
    });
});

describe("errors", () => {
    test("invalid duration-like", () => {
        const plainTime = new Temporal.PlainTime(1, 1, 1, 1, 1, 1);
        const invalidDuration = { foo: 1, bar: 2 };
        expect(() => {
            plainTime.add(invalidDuration);
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });
});
