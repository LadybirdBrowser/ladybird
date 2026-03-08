describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.prototype.withCalendar).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = "gregory";
        const firstPlainDate = new Temporal.PlainDate(1, 1, 1);
        expect(firstPlainDate.calendarId).not.toBe(calendar);
        const secondPlainDate = firstPlainDate.withCalendar(calendar);
        expect(secondPlainDate.calendarId).toBe(calendar);
    });

    test("switching to hebrew calendar reinterprets fields", () => {
        const isoDate = new Temporal.PlainDate(2024, 6, 15);
        const hebrewDate = isoDate.withCalendar("hebrew");
        expect(hebrewDate.calendarId).toBe("hebrew");
        expect(hebrewDate.year).toBe(5784);
        // The underlying ISO date is preserved.
        expect(hebrewDate.toString()).toBe("2024-06-15[u-ca=hebrew]");
    });
});

describe("errors", () => {
    test("invalid calendar identifier", () => {
        const date = new Temporal.PlainDate(2024, 1, 1);
        expect(() => {
            date.withCalendar("invalid-calendar");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'invalid-calendar'");
    });
});
