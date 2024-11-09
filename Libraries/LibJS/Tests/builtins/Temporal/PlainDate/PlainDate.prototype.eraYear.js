describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(plainDate.eraYear).toBeUndefined();
    });

    test("calendar with custom eraYear function", () => {
        const calendar = {
            eraYear() {
                return 123;
            },
        };
        const plainDate = new Temporal.PlainDate(2021, 7, 6, calendar);
        expect(plainDate.eraYear).toBe(123);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
