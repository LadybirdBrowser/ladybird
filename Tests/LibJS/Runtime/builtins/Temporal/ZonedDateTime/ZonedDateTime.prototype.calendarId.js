describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const timeZone = "UTC";
        const calendar = "gregory";
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone, calendar);
        expect(zonedDateTime.calendarId).toBe("gregory");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
