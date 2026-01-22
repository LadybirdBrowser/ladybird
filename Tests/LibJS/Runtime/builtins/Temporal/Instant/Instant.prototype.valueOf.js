describe("errors", () => {
    test("throws TypeError", () => {
        expect(() => {
            new Temporal.Instant(0n).valueOf();
        }).toThrowWithMessage(TypeError, "Cannot convert Temporal.Instant to a primitive value");
    });
});
