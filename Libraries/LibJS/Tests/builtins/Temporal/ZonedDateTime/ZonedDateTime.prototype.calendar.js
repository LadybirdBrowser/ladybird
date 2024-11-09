describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = new Temporal.Calendar("iso8601");
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone, calendar);
        expect(zonedDateTime.calendar).toBe(calendar);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "calendar", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
