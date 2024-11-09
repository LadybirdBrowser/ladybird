describe("errors", () => {
    test("throws TypeError", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        expect(() => {
            new Temporal.ZonedDateTime(0n, timeZone).valueOf();
        }).toThrowWithMessage(
            TypeError,
            "Cannot convert Temporal.ZonedDateTime to a primitive value"
        );
    });
});
