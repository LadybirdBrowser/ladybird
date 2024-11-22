describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(plainDate.eraYear).toBeUndefined();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
