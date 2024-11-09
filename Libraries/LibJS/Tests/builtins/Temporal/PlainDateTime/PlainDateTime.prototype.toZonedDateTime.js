describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.toZonedDateTime).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47, 123, 456, 789);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(7);
        expect(zonedDateTime.day).toBe(6);
        expect(zonedDateTime.hour).toBe(18);
        expect(zonedDateTime.minute).toBe(14);
        expect(zonedDateTime.second).toBe(47);
        expect(zonedDateTime.millisecond).toBe(123);
        expect(zonedDateTime.microsecond).toBe(456);
        expect(zonedDateTime.nanosecond).toBe(789);
        expect(zonedDateTime.calendar).toBe(plainDateTime.calendar);
        expect(zonedDateTime.timeZone).toBe(timeZone);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.toZonedDateTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
