describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.withTimeZone).toHaveLength(1);
    });

    test("basic functionality", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "America/New_York");
        expect(zonedDateTime.timeZoneId).toBe("America/New_York");

        const withTimeZoneZonedDateTime = zonedDateTime.withTimeZone("UTC");
        expect(withTimeZoneZonedDateTime.timeZoneId).toBe("UTC");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.withTimeZone.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("from invalid time zone string", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC");

        expect(() => {
            zonedDateTime.withTimeZone("UTCfoobar");
        }).toThrowWithMessage(RangeError, "Invalid time zone name 'UTCfoobar'");
    });
});
