describe("errors", () => {
    test("throws TypeError", () => {
        expect(() => {
            new Temporal.PlainDateTime(2021, 7, 22, 19, 54, 38).valueOf();
        }).toThrowWithMessage(
            TypeError,
            "Cannot convert Temporal.PlainDateTime to a primitive value"
        );
    });
});
