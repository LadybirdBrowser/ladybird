describe("correct behavior", () => {
    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        expect(zonedDateTime.eraYear).toBeUndefined();
    });

    test("calendar with custom eraYear function", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = {
            eraYear() {
                return 123;
            },
        };
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone, calendar);
        expect(zonedDateTime.eraYear).toBe(123);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
