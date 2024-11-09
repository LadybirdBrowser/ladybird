describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.fromEpochNanoseconds).toHaveLength(1);
    });

    test("basic functionality", () => {
        expect(Temporal.Instant.fromEpochNanoseconds(0n).epochNanoseconds).toBe(0n);
        expect(Temporal.Instant.fromEpochNanoseconds(1n).epochNanoseconds).toBe(1n);
        expect(Temporal.Instant.fromEpochNanoseconds(999_999_999n).epochNanoseconds).toBe(
            999_999_999n
        );
        expect(
            Temporal.Instant.fromEpochNanoseconds(8_640_000_000_000_000_000_000n).epochNanoseconds
        ).toBe(8_640_000_000_000_000_000_000n);

        expect(Temporal.Instant.fromEpochNanoseconds(-0n).epochNanoseconds).toBe(0n);
        expect(Temporal.Instant.fromEpochNanoseconds(-1n).epochNanoseconds).toBe(-1n);
        expect(Temporal.Instant.fromEpochNanoseconds(-999_999_999n).epochNanoseconds).toBe(
            -999_999_999n
        );
        expect(
            Temporal.Instant.fromEpochNanoseconds(-8_640_000_000_000_000_000_000n).epochNanoseconds
        ).toBe(-8_640_000_000_000_000_000_000n);
    });
});

describe("errors", () => {
    test("argument must be coercible to BigInt", () => {
        expect(() => {
            Temporal.Instant.fromEpochNanoseconds(123);
        }).toThrowWithMessage(TypeError, "Cannot convert number to BigInt");
        expect(() => {
            Temporal.Instant.fromEpochNanoseconds("foo");
        }).toThrowWithMessage(SyntaxError, "Invalid value for BigInt: foo");
    });

    test("out-of-range epoch nanoseconds value", () => {
        expect(() => {
            Temporal.Instant.fromEpochNanoseconds(8_640_000_000_000_000_000_001n);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
        expect(() => {
            Temporal.Instant.fromEpochNanoseconds(-8_640_000_000_000_000_000_001n);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });
});
