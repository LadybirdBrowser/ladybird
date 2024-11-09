describe("errors", () => {
    test("throws TypeError", () => {
        expect(() => {
            new Temporal.PlainTime(19, 54, 38).valueOf();
        }).toThrowWithMessage(TypeError, "Cannot convert Temporal.PlainTime to a primitive value");
    });
});
