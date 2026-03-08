describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.eraYear).toBeUndefined();
    });

    test("gregory calendar", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, "UTC", "gregory");
        expect(zonedDateTime.eraYear).toBe(2021);
    });

    test("japanese calendar era year", () => {
        const reiwa = Temporal.ZonedDateTime.from({
            year: 2024,
            month: 6,
            day: 15,
            timeZone: "UTC",
            calendar: "japanese",
        });
        expect(reiwa.eraYear).toBe(6);

        const heisei = Temporal.ZonedDateTime.from({
            year: 2000,
            month: 1,
            day: 1,
            timeZone: "UTC",
            calendar: "japanese",
        });
        expect(heisei.eraYear).toBe(12);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
