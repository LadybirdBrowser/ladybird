describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const calendar = "iso8601";
        const plainDateTime = new Temporal.PlainDateTime(2000, 5, 1, 12, 30, 0, calendar);
        expect(plainDateTime.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDateTime.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
