describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDate.prototype.toPlainYearMonth).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const plainYearMonth = plainDate.toPlainYearMonth();
        expect(plainYearMonth.calendarId).toBe(plainDate.calendarId);
        expect(plainYearMonth.year).toBe(2021);
        expect(plainYearMonth.month).toBe(7);
    });
});
