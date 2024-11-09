describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.toPlainYearMonth).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47);
        const plainYearMonth = plainDateTime.toPlainYearMonth();
        expect(plainYearMonth.calendar).toBe(plainDateTime.calendar);
        expect(plainYearMonth.year).toBe(2021);
        expect(plainYearMonth.month).toBe(7);
    });
});
