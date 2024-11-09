describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.toPlainMonthDay).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47);
        const plainMonthDay = plainDateTime.toPlainMonthDay();
        expect(plainMonthDay.calendar).toBe(plainDateTime.calendar);
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);
    });
});
