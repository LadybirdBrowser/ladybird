describe("errors", () => {
    test("throws TypeError", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(() => {
            plainYearMonth.valueOf();
        }).toThrowWithMessage(
            TypeError,
            "Cannot convert Temporal.PlainYearMonth to a primitive value"
        );
    });
});
