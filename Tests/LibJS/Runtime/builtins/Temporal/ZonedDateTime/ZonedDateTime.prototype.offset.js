describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone);
        expect(zonedDateTime.offset).toBe("+00:00");
    });

    test("custom offset", () => {
        const timeZone = "+01:30";
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone);
        expect(zonedDateTime.offset).toBe(timeZone);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "offset", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
