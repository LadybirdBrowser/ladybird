describe("correct behavior", () => {
    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        expect(calendar.id).toBe("iso8601");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Calendar object", () => {
        expect(() => {
            Reflect.get(Temporal.Calendar.prototype, "id", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Calendar");
    });
});
