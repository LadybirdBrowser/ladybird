describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toPlainDateTime).toHaveLength(0);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        const plainDateTime = zonedDateTime.toPlainDateTime();
        expect(plainDateTime).toBeInstanceOf(Temporal.PlainDateTime);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(6);
        expect(plainDateTime.hour).toBe(23);
        expect(plainDateTime.minute).toBe(42);
        expect(plainDateTime.second).toBe(1);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toPlainDateTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
