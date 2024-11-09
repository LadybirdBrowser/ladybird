describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.withCalendar).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = { hello: "friends" };
        const firstPlainDateTime = new Temporal.PlainDateTime(1, 2, 3);
        expect(firstPlainDateTime.calendar).not.toBe(calendar);
        const secondPlainDateTime = firstPlainDateTime.withCalendar(calendar);
        expect(secondPlainDateTime.calendar).toBe(calendar);
    });
});
