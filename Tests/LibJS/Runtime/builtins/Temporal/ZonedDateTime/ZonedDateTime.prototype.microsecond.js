describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(123000n, timeZone);
        expect(zonedDateTime.microsecond).toBe(123);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "microsecond", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
