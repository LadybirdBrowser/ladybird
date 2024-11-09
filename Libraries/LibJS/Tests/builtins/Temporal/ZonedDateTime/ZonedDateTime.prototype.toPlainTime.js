describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.toPlainTime).toHaveLength(0);
    });

    test("basic functionality", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        const plainTime = zonedDateTime.toPlainTime();
        expect(plainTime).toBeInstanceOf(Temporal.PlainTime);
        expect(plainTime.hour).toBe(23);
        expect(plainTime.minute).toBe(42);
        expect(plainTime.second).toBe(1);
        expect(plainTime.millisecond).toBe(0);
        expect(plainTime.microsecond).toBe(0);
        expect(plainTime.nanosecond).toBe(0);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.toPlainTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
