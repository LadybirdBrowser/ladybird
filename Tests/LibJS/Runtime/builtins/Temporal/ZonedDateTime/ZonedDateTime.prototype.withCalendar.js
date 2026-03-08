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

    test("switching to hebrew calendar reinterprets fields", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from("2024-06-15T12:00:00+00:00[UTC]");
        const hebrewZonedDateTime = zonedDateTime.withCalendar("hebrew");
        expect(hebrewZonedDateTime.calendarId).toBe("hebrew");
        expect(hebrewZonedDateTime.year).toBe(5784);
        // The underlying epoch nanoseconds is preserved.
        expect(hebrewZonedDateTime.epochNanoseconds).toBe(zonedDateTime.epochNanoseconds);
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
