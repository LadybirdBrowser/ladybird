describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.withCalendar).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = "gregory";
        const firstPlainDateTime = new Temporal.PlainDateTime(1, 2, 3);
        expect(firstPlainDateTime.calendarId).not.toBe(calendar);
        const secondPlainDateTime = firstPlainDateTime.withCalendar(calendar);
        expect(secondPlainDateTime.calendarId).toBe(calendar);
    });
});
