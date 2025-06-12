describe("correct behavior", () => {
    test("basic functionality", () => {
        const duration = new Temporal.Duration(123);
        expect(duration.years).toBe(123);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Reflect.get(Temporal.Duration.prototype, "years", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });
});
