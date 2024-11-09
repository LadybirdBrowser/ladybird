describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.fromEpochSeconds).toHaveLength(1);
    });

    test("basic functionality", () => {
        expect(Temporal.Instant.fromEpochSeconds(0).epochSeconds).toBe(0);
        expect(Temporal.Instant.fromEpochSeconds(1).epochSeconds).toBe(1);
        expect(Temporal.Instant.fromEpochSeconds(999_999_999).epochSeconds).toBe(999_999_999);
        expect(Temporal.Instant.fromEpochSeconds(8_640_000_000_000).epochSeconds).toBe(
            8_640_000_000_000
        );

        expect(Temporal.Instant.fromEpochSeconds(-0).epochSeconds).toBe(0);
        expect(Temporal.Instant.fromEpochSeconds(-1).epochSeconds).toBe(-1);
        expect(Temporal.Instant.fromEpochSeconds(-999_999_999).epochSeconds).toBe(-999_999_999);
        expect(Temporal.Instant.fromEpochSeconds(-8_640_000_000_000).epochSeconds).toBe(
            -8_640_000_000_000
        );
    });
});

describe("errors", () => {
    test("argument must be coercible to BigInt", () => {
        expect(() => {
            Temporal.Instant.fromEpochSeconds(1.23);
        }).toThrowWithMessage(RangeError, "Cannot convert non-integral number to BigInt");
        // NOTE: ToNumber is called on the argument first, so this is effectively NaN.
        expect(() => {
            Temporal.Instant.fromEpochSeconds("foo");
        }).toThrowWithMessage(RangeError, "Cannot convert non-integral number to BigInt");
    });

    test("out-of-range epoch seconds value", () => {
        expect(() => {
            Temporal.Instant.fromEpochSeconds(8_640_000_000_001);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
        expect(() => {
            Temporal.Instant.fromEpochSeconds(-8_640_000_000_001);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });
});
