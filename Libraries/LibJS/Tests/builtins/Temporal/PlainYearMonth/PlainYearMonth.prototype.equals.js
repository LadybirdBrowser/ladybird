describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainYearMonth.prototype.equals).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = { hello: "friends" };
        const firstPlainDateTime = new Temporal.PlainYearMonth(1, 1, calendar);
        const secondPlainDateTime = new Temporal.PlainYearMonth(0, 1, calendar);
        expect(firstPlainDateTime.equals(firstPlainDateTime)).toBeTrue();
        expect(secondPlainDateTime.equals(secondPlainDateTime)).toBeTrue();
        expect(firstPlainDateTime.equals(secondPlainDateTime)).toBeFalse();
        expect(secondPlainDateTime.equals(firstPlainDateTime)).toBeFalse();
    });
});
