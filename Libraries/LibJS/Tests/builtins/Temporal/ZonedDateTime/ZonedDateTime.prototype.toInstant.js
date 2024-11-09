describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toInstant).toHaveLength(0);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        const instant = zonedDateTime.toInstant();
        expect(instant).toBeInstanceOf(Temporal.Instant);
        expect(instant.epochNanoseconds).toBe(1625614921000000000n);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toInstant.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
