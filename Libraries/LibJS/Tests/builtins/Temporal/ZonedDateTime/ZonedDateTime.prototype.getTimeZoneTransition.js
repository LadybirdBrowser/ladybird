describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.getTimeZoneTransition).toHaveLength(1);
    });

    test("basic functionality", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, "UTC", "iso8601");
        expect(zonedDateTime.getTimeZoneTransition("next")).toBeNull();
        expect(zonedDateTime.getTimeZoneTransition("previous")).toBeNull();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.TimeZone object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.getTimeZoneTransition.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
