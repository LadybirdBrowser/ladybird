describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const calendar = "iso8601";
        const plainDate = new Temporal.PlainDate(2000, 5, 1, calendar);
        expect(plainDate.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
