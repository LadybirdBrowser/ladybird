describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = "UTC";
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone);
        expect(zonedDateTime.offsetNanoseconds).toBe(0);
    });

    test("custom offset", () => {
        const timeZone = "+01:30";
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone);
        expect(zonedDateTime.offsetNanoseconds).toBe(5400000000000);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "offsetNanoseconds", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
