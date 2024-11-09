describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.withCalendar).toHaveLength(1);
    });

    test("basic functionality", () => {
        const object = {};
        const zonedDateTime = new Temporal.ZonedDateTime(1n, {}, object);
        expect(zonedDateTime.calendar).toBe(object);

        const calendar = new Temporal.Calendar("iso8601");
        const withCalendarZonedDateTime = zonedDateTime.withCalendar(calendar);
        expect(withCalendarZonedDateTime.calendar).toBe(calendar);
    });

    test("from calendar string", () => {
        const object = {};
        const zonedDateTime = new Temporal.ZonedDateTime(1n, {}, object);
        expect(zonedDateTime.calendar).toBe(object);

        const withCalendarZonedDateTime = zonedDateTime.withCalendar("iso8601");
        expect(withCalendarZonedDateTime.calendar).toBeInstanceOf(Temporal.Calendar);
        expect(withCalendarZonedDateTime.calendar.id).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.withCalendar.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("from invalid calendar identifier", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, {}, {});

        expect(() => {
            zonedDateTime.withCalendar("iso8602foobar");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'iso8602foobar'");
    });
});
