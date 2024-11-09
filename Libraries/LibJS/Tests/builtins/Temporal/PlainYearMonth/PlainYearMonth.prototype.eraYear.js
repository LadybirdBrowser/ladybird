describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.eraYear).toBeUndefined();
    });

    test("calendar with custom eraYear function", () => {
        const calendar = {
            eraYear() {
                return 123;
            },
        };
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7, calendar);
        expect(plainYearMonth.eraYear).toBe(123);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainYearMonth.prototype, "eraYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
