describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.era).toBeUndefined();
    });

    test("gregory calendar", () => {
        const ce = new Temporal.ZonedDateTime(1625614921000000000n, "UTC", "gregory");
        expect(ce.era).toBe("ce");

        const bce = new Temporal.ZonedDateTime(-62200000000000000000n, "UTC", "gregory");
        expect(bce.era).toBe("bce");
    });

    test("japanese calendar eras", () => {
        const reiwa = Temporal.ZonedDateTime.from({
            year: 2020,
            month: 1,
            day: 1,
            timeZone: "UTC",
            calendar: "japanese",
        });
        expect(reiwa.era).toBe("reiwa");

        const heisei = Temporal.ZonedDateTime.from({
            year: 2000,
            month: 1,
            day: 1,
            timeZone: "UTC",
            calendar: "japanese",
        });
        expect(heisei.era).toBe("heisei");

        const showa = Temporal.ZonedDateTime.from({
            year: 1970,
            month: 1,
            day: 1,
            timeZone: "UTC",
            calendar: "japanese",
        });
        expect(showa.era).toBe("showa");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "era", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
