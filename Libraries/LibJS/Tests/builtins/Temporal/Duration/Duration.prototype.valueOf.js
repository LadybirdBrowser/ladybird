describe("errors", () => {
    test("throws TypeError", () => {
        expect(() => {
            new Temporal.Duration().valueOf();
        }).toThrowWithMessage(TypeError, "Cannot convert Temporal.Duration to a primitive value");
    });
});
