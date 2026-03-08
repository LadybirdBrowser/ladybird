describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.monthsInYear).toBe(12);
    });

    test("hebrew leap year has 13 months", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from("2024-04-09T12:00:00+00:00[UTC][u-ca=hebrew]");
        expect(zonedDateTime.year).toBe(5784);
        expect(zonedDateTime.monthsInYear).toBe(13);
    });

    test("hebrew non-leap year has 12 months", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from("2023-04-09T12:00:00+00:00[UTC][u-ca=hebrew]");
        expect(zonedDateTime.year).toBe(5783);
        expect(zonedDateTime.monthsInYear).toBe(12);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "monthsInYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
