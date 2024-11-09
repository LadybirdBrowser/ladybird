describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.getISOFields).toHaveLength(0);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = new Temporal.Calendar("iso8601");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone, calendar);
        const fields = zonedDateTime.getISOFields();
        expect(fields).toEqual({
            calendar: calendar,
            isoDay: 6,
            isoHour: 23,
            isoMicrosecond: 0,
            isoMillisecond: 0,
            isoMinute: 42,
            isoMonth: 7,
            isoNanosecond: 0,
            isoSecond: 1,
            isoYear: 2021,
            offset: "+00:00",
            timeZone: timeZone,
        });
        // Test field order
        expect(Object.getOwnPropertyNames(fields)).toEqual([
            "calendar",
            "isoDay",
            "isoHour",
            "isoMicrosecond",
            "isoMillisecond",
            "isoMinute",
            "isoMonth",
            "isoNanosecond",
            "isoSecond",
            "isoYear",
            "offset",
            "timeZone",
        ]);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.getISOFields.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, {});
        expect(() => {
            zonedDateTime.getISOFields();
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });
});
