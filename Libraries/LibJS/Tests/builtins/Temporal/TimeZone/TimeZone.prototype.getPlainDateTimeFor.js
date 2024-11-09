describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.TimeZone.prototype.getPlainDateTimeFor).toHaveLength(1);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const instant = Temporal.Instant.fromEpochSeconds(123456789);
        const plainDateTime = timeZone.getPlainDateTimeFor(instant);
        expect(plainDateTime.year).toBe(1973);
        expect(plainDateTime.month).toBe(11);
        expect(plainDateTime.day).toBe(29);
        expect(plainDateTime.hour).toBe(21);
        expect(plainDateTime.minute).toBe(33);
        expect(plainDateTime.second).toBe(9);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
        expect(plainDateTime.calendar.id).toBe("iso8601");
    });

    test("custom calendar", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const instant = new Temporal.Instant(0n);
        const calendar = new Temporal.Calendar("iso8601");
        const plainDateTime = timeZone.getPlainDateTimeFor(instant, calendar);
        expect(plainDateTime.calendar).toBe(calendar);
    });
});

describe("errors", () => {
    test("time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        timeZone.getOffsetNanosecondsFor = undefined;
        const instant = new Temporal.Instant(1n);
        expect(() => {
            timeZone.getPlainDateTimeFor(instant);
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });

    test("this value must be a Temporal.TimeZone object", () => {
        expect(() => {
            Temporal.TimeZone.prototype.getPlainDateTimeFor.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.TimeZone");
    });
});
