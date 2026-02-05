describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.withCalendar).toHaveLength(1);
    });

    test("basic functionality", () => {
        const object = {};
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC", "gregory");
        expect(zonedDateTime.calendarId).toBe("gregory");

        const withCalendarZonedDateTime = zonedDateTime.withCalendar("iso8601");
        expect(withCalendarZonedDateTime.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.withCalendar.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("from invalid calendar identifier", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC", "iso8601");

        expect(() => {
            zonedDateTime.withCalendar("iso8602foobar");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'iso8602foobar'");
    });
});
