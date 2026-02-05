describe("correct behavior", () => {
    test("basic functionality", () => {
        const nonBlankDuration = new Temporal.Duration(123);
        expect(nonBlankDuration.blank).toBeFalse();

        const blankDuration = new Temporal.Duration(0);
        expect(blankDuration.blank).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Reflect.get(Temporal.Duration.prototype, "blank", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });
});
