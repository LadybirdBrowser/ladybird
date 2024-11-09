describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47);
        expect(plainDateTime.eraYear).toBeUndefined();
    });

    test("calendar with custom eraYear function", () => {
        const calendar = {
            eraYear() {
                return 123;
            },
        };
        const plainDateTime = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47, 0, 0, 0, calendar);
        expect(plainDateTime.eraYear).toBe(123);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDateTime.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });
});
