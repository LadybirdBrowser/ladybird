describe("correct behavior", () => {
    test("basic functionality", () => {
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(date.monthCode).toBe("M07");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "monthCode", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
