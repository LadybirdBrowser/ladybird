describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.era).toBeUndefined();
    });

    test("calendar with custom era function", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = {
            era() {
                return "foo";
            },
        };
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone, calendar);
        expect(zonedDateTime.era).toBe("foo");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "era", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
