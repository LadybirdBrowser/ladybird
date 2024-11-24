describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDate.prototype.toPlainMonthDay).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const plainMonthDay = plainDate.toPlainMonthDay();
        expect(plainMonthDay.calendarId).toBe(plainDate.calendarId);
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);

        const fields = plainMonthDay.toString({ calendarName: "always" });
        const year = fields.split("-")[0];
        expect(year).toBe("1972");
    });
});
