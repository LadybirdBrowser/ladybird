describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(date.daysInYear).toBe(365);
    });

    test("chinese calendar days in year", () => {
        const expectedDays = {
            2020: 384,
            2021: 354,
            2022: 355,
            2023: 384,
            2024: 354,
            2025: 384,
            2026: 354,
            2027: 354,
            2028: 384,
        };

        for (const [year, days] of Object.entries(expectedDays)) {
            const date = Temporal.PlainDate.from({ year: Number(year), month: 1, day: 1, calendar: "chinese" });
            expect(date.daysInYear).toBe(days);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "daysInYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
