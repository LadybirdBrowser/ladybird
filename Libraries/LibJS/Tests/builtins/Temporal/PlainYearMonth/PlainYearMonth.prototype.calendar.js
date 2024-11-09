describe("correct behavior", () => {
    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7, calendar);
        expect(plainYearMonth.calendar).toBe(calendar);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainYearMonth.prototype, "calendar", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
