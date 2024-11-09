describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Now.zonedDateTimeISO).toHaveLength(0);
    });

    test("basic functionality", () => {
        const zonedDateTime = Temporal.Now.zonedDateTimeISO();
        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(zonedDateTime.calendar.id).toBe("iso8601");
    });

    test("with time zone", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = Temporal.Now.zonedDateTimeISO(timeZone);
        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(zonedDateTime.calendar.id).toBe("iso8601");
        expect(zonedDateTime.timeZone).toBe(timeZone);
    });
});
