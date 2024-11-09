describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.toPlainDateTime).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 29);
        const plainTime = new Temporal.PlainTime(23, 45, 12, 1, 2, 3);
        const plainDateTime = plainTime.toPlainDateTime(plainDate);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(29);
        expect(plainDateTime.hour).toBe(23);
        expect(plainDateTime.minute).toBe(45);
        expect(plainDateTime.second).toBe(12);
        expect(plainDateTime.millisecond).toBe(1);
        expect(plainDateTime.microsecond).toBe(2);
        expect(plainDateTime.nanosecond).toBe(3);
    });
});
