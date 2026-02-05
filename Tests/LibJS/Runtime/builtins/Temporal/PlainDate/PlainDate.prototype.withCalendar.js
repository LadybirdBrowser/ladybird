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
});
