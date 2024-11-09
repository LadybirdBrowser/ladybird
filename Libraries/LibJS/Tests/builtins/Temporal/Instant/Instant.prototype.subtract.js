describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.prototype.subtract).toHaveLength(1);
    });

    test("basic functionality", () => {
        const instant = new Temporal.Instant(1625614921000000000n);
        const duration = new Temporal.Duration(0, 0, 0, 0, 1, 2, 3, 4, 5, 6);
        expect(instant.subtract(duration).epochNanoseconds).toBe(1625611197995994994n);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Temporal.Instant.prototype.subtract.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });

    test("invalid nanoseconds value, positive", () => {
        const instant = new Temporal.Instant(8_640_000_000_000_000_000_000n);
        const duration = new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, -1);
        expect(() => {
            instant.subtract(duration);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });

    test("invalid nanoseconds value, negative", () => {
        const instant = new Temporal.Instant(-8_640_000_000_000_000_000_000n);
        const duration = new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
        expect(() => {
            instant.subtract(duration);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });

    test("disallowed fields", () => {
        const instant = new Temporal.Instant(1625614921000000000n);
        for (const [args, property] of [
            [[123, 0, 0, 0], "years"],
            [[0, 123, 0, 0], "months"],
            [[0, 0, 123, 0], "weeks"],
            [[0, 0, 0, 123], "days"],
        ]) {
            const duration = new Temporal.Duration(...args);
            expect(() => {
                instant.subtract(duration);
            }).toThrowWithMessage(
                RangeError,
                `Invalid value for duration property '${property}': must be zero, got 123`
            );
        }
    });
});
