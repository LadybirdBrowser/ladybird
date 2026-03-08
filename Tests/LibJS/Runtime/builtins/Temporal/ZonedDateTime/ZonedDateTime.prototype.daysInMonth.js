describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.daysInMonth).toBe(31);
    });

    test("islamic calendar months alternate 30 and 29 days", () => {
        // Islamic month 1 (Muharram) has 30 days.
        const zonedDateTime = Temporal.ZonedDateTime.from("2024-07-08T12:00:00+00:00[UTC][u-ca=islamic-civil]");
        expect(zonedDateTime.month).toBe(1);
        expect(zonedDateTime.daysInMonth).toBe(30);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "daysInMonth", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
