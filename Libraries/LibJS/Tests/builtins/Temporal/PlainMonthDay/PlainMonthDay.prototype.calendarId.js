describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(5, 1, "iso8601");
        expect(plainMonthDay.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainMonthDay.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });
});
