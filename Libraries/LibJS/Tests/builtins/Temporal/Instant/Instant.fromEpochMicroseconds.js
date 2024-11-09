describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.fromEpochMicroseconds).toHaveLength(1);
    });

    test("basic functionality", () => {
        expect(Temporal.Instant.fromEpochMicroseconds(0n).epochMicroseconds).toBe(0n);
        expect(Temporal.Instant.fromEpochMicroseconds(1n).epochMicroseconds).toBe(1n);
        expect(Temporal.Instant.fromEpochMicroseconds(999_999_999n).epochMicroseconds).toBe(
            999_999_999n
        );
        expect(
            Temporal.Instant.fromEpochMicroseconds(8_640_000_000_000_000_000n).epochMicroseconds
        ).toBe(8_640_000_000_000_000_000n);

        expect(Temporal.Instant.fromEpochMicroseconds(-0n).epochMicroseconds).toBe(0n);
        expect(Temporal.Instant.fromEpochMicroseconds(-1n).epochMicroseconds).toBe(-1n);
        expect(Temporal.Instant.fromEpochMicroseconds(-999_999_999n).epochMicroseconds).toBe(
            -999_999_999n
        );
        expect(
            Temporal.Instant.fromEpochMicroseconds(-8_640_000_000_000_000_000n).epochMicroseconds
        ).toBe(-8_640_000_000_000_000_000n);
    });
});

describe("errors", () => {
    test("argument must be coercible to BigInt", () => {
        expect(() => {
            Temporal.Instant.fromEpochMicroseconds(123);
        }).toThrowWithMessage(TypeError, "Cannot convert number to BigInt");
        expect(() => {
            Temporal.Instant.fromEpochMicroseconds("foo");
        }).toThrowWithMessage(SyntaxError, "Invalid value for BigInt: foo");
    });

    test("out-of-range epoch microseconds value", () => {
        expect(() => {
            Temporal.Instant.fromEpochMicroseconds(8_640_000_000_000_000_001n);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
        expect(() => {
            Temporal.Instant.fromEpochMicroseconds(-8_640_000_000_000_000_001n);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });
});
