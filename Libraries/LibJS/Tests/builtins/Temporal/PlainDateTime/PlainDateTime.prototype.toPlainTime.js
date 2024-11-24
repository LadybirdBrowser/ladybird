describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.toPlainTime).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 31, 0, 32, 18, 123, 456, 789);
        const plainTime = plainDateTime.toPlainTime();
        expect(plainTime.hour).toBe(0);
        expect(plainTime.minute).toBe(32);
        expect(plainTime.second).toBe(18);
        expect(plainTime.millisecond).toBe(123);
        expect(plainTime.microsecond).toBe(456);
        expect(plainTime.nanosecond).toBe(789);
    });
});
