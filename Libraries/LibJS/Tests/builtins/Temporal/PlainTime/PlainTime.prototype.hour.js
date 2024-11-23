describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(12);
        expect(plainTime.hour).toBe(12);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainTime.prototype, "hour", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });
});
