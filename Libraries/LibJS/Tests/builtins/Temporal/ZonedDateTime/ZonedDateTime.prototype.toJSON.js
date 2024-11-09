describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toJSON).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 1, 33, 5, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        expect(zonedDateTime.toJSON()).toBe("2021-11-03T01:33:05.1002003+00:00[UTC]");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toJSON.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, {});
        expect(() => {
            zonedDateTime.toJSON();
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });
});
