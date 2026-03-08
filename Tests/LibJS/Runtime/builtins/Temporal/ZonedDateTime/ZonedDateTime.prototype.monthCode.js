describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.monthCode).toBe("M07");
    });

    test("gregory calendar month codes", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from({
            year: 2024,
            month: 1,
            day: 1,
            timeZone: "UTC",
            calendar: "gregory",
        });
        expect(zonedDateTime.monthCode).toBe("M01");
    });

    test("hebrew calendar leap month code", () => {
        // 2024-02-11 falls in Adar I (M05L) of Hebrew year 5784 (a leap year).
        const zonedDateTime = Temporal.ZonedDateTime.from("2024-02-11T12:00:00+00:00[UTC][u-ca=hebrew]");
        expect(zonedDateTime.monthCode).toBe("M05L");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "monthCode", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
