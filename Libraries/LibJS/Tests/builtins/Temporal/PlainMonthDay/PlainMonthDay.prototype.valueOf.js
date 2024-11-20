describe("errors", () => {
    test("throws TypeError", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(() => {
            plainMonthDay.valueOf();
        }).toThrowWithMessage(
            TypeError,
            "Cannot convert Temporal.PlainMonthDay to a primitive value"
        );
    });
});
