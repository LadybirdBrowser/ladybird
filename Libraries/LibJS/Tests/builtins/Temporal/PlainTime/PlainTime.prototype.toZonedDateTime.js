describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.toZonedDateTime).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(18, 14, 47, 123, 456, 789);
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainTime.toZonedDateTime({ plainDate, timeZone });
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(18);
        expect(zonedDateTime.minute).toBe(14);
        expect(zonedDateTime.second).toBe(47);
        expect(zonedDateTime.millisecond).toBe(123);
        expect(zonedDateTime.microsecond).toBe(456);
        expect(zonedDateTime.nanosecond).toBe(789);
        expect(zonedDateTime.calendar).toBe(plainDate.calendar);
        expect(zonedDateTime.timeZone).toBe(timeZone);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.toZonedDateTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });

    test("item argument must be an object", () => {
        const plainTime = new Temporal.PlainTime();
        for (const value of [123, NaN, Infinity, true, false, null, undefined]) {
            expect(() => {
                plainTime.toZonedDateTime(value);
            }).toThrowWithMessage(TypeError, `${value} is not an object`);
        }
    });

    test("item argument must have a 'plainDate' property", () => {
        const plainTime = new Temporal.PlainTime();
        expect(() => {
            plainTime.toZonedDateTime({});
        }).toThrowWithMessage(TypeError, "Required property plainDate is missing or undefined");
    });

    test("item argument must have a 'timeZone' property", () => {
        const plainDate = new Temporal.PlainDate(1970, 1, 1);
        const plainTime = new Temporal.PlainTime();
        expect(() => {
            plainTime.toZonedDateTime({ plainDate });
        }).toThrowWithMessage(TypeError, "Required property timeZone is missing or undefined");
    });
});
