describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.ZonedDateTime.compare).toHaveLength(2);
    });

    test("basic functionality", () => {
        const zonedDateTimeOne = new Temporal.ZonedDateTime(1n, new Temporal.TimeZone("UTC"));
        const zonedDateTimeTwo = new Temporal.ZonedDateTime(2n, new Temporal.TimeZone("UTC"));

        expect(Temporal.ZonedDateTime.compare(zonedDateTimeOne, zonedDateTimeOne)).toBe(0);
        expect(Temporal.ZonedDateTime.compare(zonedDateTimeTwo, zonedDateTimeTwo)).toBe(0);
        expect(Temporal.ZonedDateTime.compare(zonedDateTimeOne, zonedDateTimeTwo)).toBe(-1);
        expect(Temporal.ZonedDateTime.compare(zonedDateTimeTwo, zonedDateTimeOne)).toBe(1);
    });
});
