describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Now.zonedDateTime).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const zonedDateTime = Temporal.Now.zonedDateTime(calendar);
        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(zonedDateTime.calendar).toBe(calendar);
    });

    test("with time zone", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = Temporal.Now.zonedDateTime(calendar, timeZone);
        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(zonedDateTime.calendar).toBe(calendar);
        expect(zonedDateTime.timeZone).toBe(timeZone);
    });
});
