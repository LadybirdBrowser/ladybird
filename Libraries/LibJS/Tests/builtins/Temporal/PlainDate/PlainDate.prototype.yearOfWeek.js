describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2023, 1, 1);
        expect(date.yearOfWeek).toBe(2022);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "yearOfWeek", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
