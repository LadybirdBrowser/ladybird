describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainMonthDay.prototype.equals).toHaveLength(1);
    });

    test("basic functionality", () => {
        const firstPlainMonthDay = new Temporal.PlainMonthDay(2, 1, "iso8601");
        const secondPlainMonthDay = new Temporal.PlainMonthDay(1, 1, "iso8601");
        expect(firstPlainMonthDay.equals(firstPlainMonthDay)).toBeTrue();
        expect(secondPlainMonthDay.equals(secondPlainMonthDay)).toBeTrue();
        expect(firstPlainMonthDay.equals(secondPlainMonthDay)).toBeFalse();
        expect(secondPlainMonthDay.equals(firstPlainMonthDay)).toBeFalse();
    });
});
