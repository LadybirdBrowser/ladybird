describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.prototype.toZonedDateTime).toHaveLength(1);
    });

    test("basic functionality - time zone", () => {
        // 3.b. in the spec
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const timeZone = "UTC";
        const zonedDateTime = plainDate.toZonedDateTime(timeZone);
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(0);
        expect(zonedDateTime.minute).toBe(0);
        expect(zonedDateTime.second).toBe(0);
        expect(zonedDateTime.millisecond).toBe(0);
        expect(zonedDateTime.microsecond).toBe(0);
        expect(zonedDateTime.nanosecond).toBe(0);
        expect(zonedDateTime.calendarId).toBe(plainDate.calendarId);
        expect(zonedDateTime.timeZoneId).toBe(timeZone);
    });

    test("basic functionality - time zone like object", () => {
        // 3.c. in the spec
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const timeZone = "UTC";
        const zonedDateTime = plainDate.toZonedDateTime(timeZone);
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(0);
        expect(zonedDateTime.minute).toBe(0);
        expect(zonedDateTime.second).toBe(0);
        expect(zonedDateTime.millisecond).toBe(0);
        expect(zonedDateTime.microsecond).toBe(0);
        expect(zonedDateTime.nanosecond).toBe(0);
        expect(zonedDateTime.calendarId).toBe(plainDate.calendarId);
        expect(zonedDateTime.timeZoneId).toBe(timeZone);
    });

    test("basic functionality - time zone like object and plain time", () => {
        // 3.c. in the spec
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);
        const timeZone = "UTC";
        const zonedDateTime = plainDate.toZonedDateTime({ timeZone, plainTime });
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(18);
        expect(zonedDateTime.minute).toBe(14);
        expect(zonedDateTime.second).toBe(47);
        expect(zonedDateTime.millisecond).toBe(123);
        expect(zonedDateTime.microsecond).toBe(456);
        expect(zonedDateTime.nanosecond).toBe(789);
        expect(zonedDateTime.calendarId).toBe(plainDate.calendarId);
        expect(zonedDateTime.timeZoneId).toBe(timeZone);
    });

    test("basic functionality - time zone identifier", () => {
        // 4. in the spec
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const zonedDateTime = plainDate.toZonedDateTime("UTC");
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(0);
        expect(zonedDateTime.minute).toBe(0);
        expect(zonedDateTime.second).toBe(0);
        expect(zonedDateTime.millisecond).toBe(0);
        expect(zonedDateTime.microsecond).toBe(0);
        expect(zonedDateTime.nanosecond).toBe(0);
        expect(zonedDateTime.calendarId).toBe(plainDate.calendarId);
        expect(zonedDateTime.timeZoneId).toBe("UTC");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.toZonedDateTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
