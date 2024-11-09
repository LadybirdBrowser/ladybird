describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(plainDate.era).toBeUndefined();
    });

    test("calendar with custom era function", () => {
        const calendar = {
            era() {
                return "foo";
            },
        };
        const plainDate = new Temporal.PlainDate(2021, 7, 6, calendar);
        expect(plainDate.era).toBe("foo");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "era", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
