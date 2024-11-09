describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 30);
        expect(plainDateTime.weekOfYear).toBe(30);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDateTime.prototype, "weekOfYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
