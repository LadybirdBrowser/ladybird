describe("correct behavior", () => {
    test("basic functionality", () => {
        const calendar = { hello: "friends" };
        const plainDate = new Temporal.PlainDate(1, 1, 1, calendar);
        expect(plainDate.calendar).toBe(calendar);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "calendar", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
