describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Instant.prototype.toZonedDateTime).toHaveLength(1);
    });

    test("basic functionality", () => {
        const instant = new Temporal.Instant(1625614921123456789n);
        const calendar = new Temporal.Calendar("iso8601");
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = instant.toZonedDateTime({ calendar, timeZone });
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(23);
        expect(zonedDateTime.minute).toBe(42);
        expect(zonedDateTime.second).toBe(1);
        expect(zonedDateTime.millisecond).toBe(123);
        expect(zonedDateTime.microsecond).toBe(456);
        expect(zonedDateTime.nanosecond).toBe(789);
        expect(zonedDateTime.calendar).toBe(calendar);
        expect(zonedDateTime.timeZone).toBe(timeZone);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Instant object", () => {
        expect(() => {
            Temporal.Instant.prototype.toZonedDateTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Instant");
    });

    test("item argument must be an object", () => {
        const instant = new Temporal.Instant(0n);
        for (const value of [123, NaN, Infinity, true, false, null, undefined]) {
            expect(() => {
                instant.toZonedDateTime(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        }
    });

    test("item argument must have a 'calendar' property", () => {
        const instant = new Temporal.Instant(0n);
        expect(() => {
            instant.toZonedDateTime({});
        }).toThrowWithMessage(TypeError, "Required property calendar is missing or undefined");
    });

    test("item argument must have a 'timeZone' property", () => {
        const instant = new Temporal.Instant(0n);
        expect(() => {
            instant.toZonedDateTime({ calendar: {} });
        }).toThrowWithMessage(TypeError, "Required property timeZone is missing or undefined");
    });
});
