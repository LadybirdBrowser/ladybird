describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.prototype.toZonedDateTimeISO).toHaveLength(1);
    });

    test("basic functionality", () => {
        const instant = new Temporal.Instant(1625614921123456789n);
        const zonedDateTime = instant.toZonedDateTimeISO("UTC");
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(23);
        expect(zonedDateTime.minute).toBe(42);
        expect(zonedDateTime.second).toBe(1);
        expect(zonedDateTime.millisecond).toBe(123);
        expect(zonedDateTime.microsecond).toBe(456);
        expect(zonedDateTime.nanosecond).toBe(789);
        expect(zonedDateTime.calendar.id).toBe("iso8601");
        expect(zonedDateTime.timeZone.id).toBe("UTC");
    });

    test("custom time zone object", () => {
        const instant = new Temporal.Instant(1625614921123456789n);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = instant.toZonedDateTimeISO({ timeZone });
        expect(zonedDateTime.timeZone).toBe(timeZone);
    });

    test("avoids extra timeZone property lookup", () => {
        const instant = new Temporal.Instant(1625614921123456789n);

        let timesGetterCalled = 0;
        const timeZoneObject = {
            get timeZone() {
                timesGetterCalled++;
                return "UTC";
            },

            toString() {
                return "UTC";
            },
        };

        instant.toZonedDateTimeISO({ timeZone: timeZoneObject });
        expect(timesGetterCalled).toBe(0);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Temporal.Instant.prototype.toZonedDateTimeISO.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });
});
