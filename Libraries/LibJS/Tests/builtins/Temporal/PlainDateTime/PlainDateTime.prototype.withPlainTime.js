describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.withPlainTime).toHaveLength(0);
    });

    test("basic functionality", () => {
        const firstPlainDateTime = new Temporal.PlainDateTime(1, 2, 3, 4, 5, 6, 7, 8, 9);
        const plainTime = new Temporal.PlainTime(10, 11, 12, 13, 14, 15);
        const secondPlainDateTime = firstPlainDateTime.withPlainTime(plainTime);
        expect(secondPlainDateTime.year).toBe(1);
        expect(secondPlainDateTime.month).toBe(2);
        expect(secondPlainDateTime.day).toBe(3);
        expect(secondPlainDateTime.hour).toBe(10);
        expect(secondPlainDateTime.minute).toBe(11);
        expect(secondPlainDateTime.second).toBe(12);
        expect(secondPlainDateTime.millisecond).toBe(13);
        expect(secondPlainDateTime.microsecond).toBe(14);
        expect(secondPlainDateTime.nanosecond).toBe(15);
    });
});
