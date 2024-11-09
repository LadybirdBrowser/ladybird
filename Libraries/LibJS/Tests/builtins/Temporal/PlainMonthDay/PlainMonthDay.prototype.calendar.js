describe("correct behavior", () => {
    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6, calendar);
        expect(plainMonthDay.calendar).toBe(calendar);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainMonthDay.prototype, "calendar", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });
});
