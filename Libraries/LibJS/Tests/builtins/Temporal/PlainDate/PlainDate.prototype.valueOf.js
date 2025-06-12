describe("errors", () => {
    test("throws TypeError", () => {
        expect(() => {
            new Temporal.PlainDate(2021, 7, 21).valueOf();
        }).toThrowWithMessage(TypeError, "Cannot convert Temporal.PlainDate to a primitive value");
    });
});
