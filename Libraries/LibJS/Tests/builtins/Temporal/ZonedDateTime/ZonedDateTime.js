describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Temporal.ZonedDateTime();
        }).toThrowWithMessage(
            TypeError,
            "Temporal.ZonedDateTime constructor must be called with 'new'"
        );
    });

    test("out-of-range epoch nanoseconds value", () => {
        expect(() => {
            new Temporal.ZonedDateTime(8_640_000_000_000_000_000_001n);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
        expect(() => {
            new Temporal.ZonedDateTime(-8_640_000_000_000_000_000_001n);
        }).toThrowWithMessage(
            RangeError,
            "Invalid epoch nanoseconds value, must be in range -86400 * 10^17 to 86400 * 10^17"
        );
    });
});

describe("normal behavior", () => {
    test("length is 2", () => {
        expect(Temporal.ZonedDateTime).toHaveLength(2);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone);
        expect(typeof zonedDateTime).toBe("object");
        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(Object.getPrototypeOf(zonedDateTime)).toBe(Temporal.ZonedDateTime.prototype);
    });
});
