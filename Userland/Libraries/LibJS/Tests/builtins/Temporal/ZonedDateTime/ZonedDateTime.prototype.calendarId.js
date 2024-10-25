describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const calendar = "iso8601";
        const zonedDateTime = new Temporal.ZonedDateTime(
            0n,
            Temporal.TimeZone.from("UTC"),
            Temporal.Calendar.from(calendar)
        );
        expect(zonedDateTime.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
