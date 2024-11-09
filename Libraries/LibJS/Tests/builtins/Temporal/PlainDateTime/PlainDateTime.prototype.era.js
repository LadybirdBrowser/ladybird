describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47);
        expect(plainDateTime.era).toBeUndefined();
    });

    test("calendar with custom era function", () => {
        const calendar = {
            era() {
                return "foo";
            },
        };
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47, 0, 0, 0, calendar);
        expect(plainDateTime.era).toBe("foo");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDateTime.prototype, "era", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
