describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.fromEpochMilliseconds).toHaveLength(1);
    });

    test("basic functionality", () => {
        expect(Temporal.Instant.fromEpochMilliseconds(0).epochMilliseconds).toBe(0);
        expect(Temporal.Instant.fromEpochMilliseconds(1).epochMilliseconds).toBe(1);
        expect(Temporal.Instant.fromEpochMilliseconds(999_999_999).epochMilliseconds).toBe(
            999_999_999
        );
        expect(
            Temporal.Instant.fromEpochMilliseconds(8_640_000_000_000_000).epochMilliseconds
        ).toBe(8_640_000_000_000_000);

        expect(Temporal.Instant.fromEpochMilliseconds(-0).epochMilliseconds).toBe(0);
        expect(Temporal.Instant.fromEpochMilliseconds(-1).epochMilliseconds).toBe(-1);
        expect(Temporal.Instant.fromEpochMilliseconds(-999_999_999).epochMilliseconds).toBe(
            -999_999_999
        );
        expect(
            Temporal.Instant.fromEpochMilliseconds(-8_640_000_000_000_000).epochMilliseconds
        ).toBe(-8_640_000_000_000_000);
    });
});

describe("errors", () => {
    test("argument must be coercible to BigInt", () => {
        expect(() => {
            Temporal.Instant.fromEpochMilliseconds(1.23);
        }).toThrowWithMessage(RangeError, "Cannot convert non-integral number to BigInt");
        // NOTE: ToNumber is called on the argument first, so this is effectively NaN.
        expect(() => {
            Temporal.Instant.fromEpochMilliseconds("foo");
        }).toThrowWithMessage(RangeError, "Cannot convert non-integral number to BigInt");
    });

    test("out-of-range epoch milliseconds value", () => {
        expect(() => {
            Temporal.Instant.fromEpochMilliseconds(8_640_000_000_000_001);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
        expect(() => {
            Temporal.Instant.fromEpochMilliseconds(-8_640_000_000_000_001);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });
});
