describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.prototype.add).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(1970, 1, 1);
        const result = plainDate.add(new Temporal.Duration(51, 6, 0, 5));
        expect(result.equals(new Temporal.PlainDate(2021, 7, 6))).toBeTrue();
    });

    test("add months in gregory calendar", () => {
        const plainDate = Temporal.PlainDate.from({ year: 2024, month: 1, day: 31, calendar: "gregory" });
        // Adding 1 month to Jan 31 should constrain to Feb 29 (2024 is a leap year).
        const result = plainDate.add({ months: 1 });
        expect(result.month).toBe(2);
        expect(result.day).toBe(29);
    });

    test("add years in hebrew calendar", () => {
        const plainDate = Temporal.PlainDate.from("2024-04-09[u-ca=hebrew]");
        expect(plainDate.year).toBe(5784);

        const result = plainDate.add({ years: 1 });
        expect(result.year).toBe(5785);
        expect(result.calendarId).toBe("hebrew");
    });

    test("add months across year boundary in japanese calendar", () => {
        const plainDate = Temporal.PlainDate.from({ year: 2024, month: 11, day: 15, calendar: "japanese" });
        const result = plainDate.add({ months: 3 });
        expect(result.month).toBe(2);
        expect(result.era).toBe("reiwa");
        expect(result.eraYear).toBe(7);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.add.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
