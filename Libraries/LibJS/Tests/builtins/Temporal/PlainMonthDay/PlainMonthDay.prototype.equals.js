describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainMonthDay.prototype.equals).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = { hello: "friends" };
        const firstPlainMonthDay = new Temporal.PlainMonthDay(2, 1, calendar);
        const secondPlainMonthDay = new Temporal.PlainMonthDay(1, 1, calendar);
        expect(firstPlainMonthDay.equals(firstPlainMonthDay)).toBeTrue();
        expect(secondPlainMonthDay.equals(secondPlainMonthDay)).toBeTrue();
        expect(firstPlainMonthDay.equals(secondPlainMonthDay)).toBeFalse();
        expect(secondPlainMonthDay.equals(firstPlainMonthDay)).toBeFalse();
    });
});
