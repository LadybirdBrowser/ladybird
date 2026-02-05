describe("correct behavior", () => {
    test("basic functionality", () => {
        const positiveDuration = new Temporal.Duration(123);
        expect(positiveDuration.sign).toBe(1);

        const negativeDuration = new Temporal.Duration(-123);
        expect(negativeDuration.sign).toBe(-1);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Reflect.get(Temporal.Duration.prototype, "sign", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });
});
