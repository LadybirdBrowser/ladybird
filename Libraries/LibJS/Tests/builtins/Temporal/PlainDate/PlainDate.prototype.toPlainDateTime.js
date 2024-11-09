describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDate.prototype.toPlainDateTime).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 8, 27);
        const plainTime = new Temporal.PlainTime(18, 11, 44, 1, 2, 3);
        const plainDateTime = plainDate.toPlainDateTime(plainTime);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(8);
        expect(plainDateTime.day).toBe(27);
        expect(plainDateTime.hour).toBe(18);
        expect(plainDateTime.minute).toBe(11);
        expect(plainDateTime.second).toBe(44);
        expect(plainDateTime.millisecond).toBe(1);
        expect(plainDateTime.microsecond).toBe(2);
        expect(plainDateTime.nanosecond).toBe(3);
    });
});
