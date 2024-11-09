describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.withTimeZone).toHaveLength(1);
    });

    test("basic functionality", () => {
        const object = {};
        const zonedDateTime = new Temporal.ZonedDateTime(1n, object);
        expect(zonedDateTime.timeZone).toBe(object);

        const timeZone = new Temporal.TimeZone("UTC");
        const withTimeZoneZonedDateTime = zonedDateTime.withTimeZone(timeZone);
        expect(withTimeZoneZonedDateTime.timeZone).toBe(timeZone);
    });

    test("from time zone string", () => {
        const object = {};
        const zonedDateTime = new Temporal.ZonedDateTime(1n, object);
        expect(zonedDateTime.timeZone).toBe(object);

        const withTimeZoneZonedDateTime = zonedDateTime.withTimeZone("UTC");
        expect(withTimeZoneZonedDateTime.timeZone).toBeInstanceOf(Temporal.TimeZone);
        expect(withTimeZoneZonedDateTime.timeZone.id).toBe("UTC");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.withTimeZone.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("from invalid time zone string", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, {});

        expect(() => {
            zonedDateTime.withTimeZone("UTCfoobar");
        }).toThrowWithMessage(RangeError, "Invalid time zone name 'UTCfoobar'");
    });
});
