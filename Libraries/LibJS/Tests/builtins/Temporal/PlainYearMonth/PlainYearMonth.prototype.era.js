describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.era).toBeUndefined();
    });

    test("calendar with custom era function", () => {
        const calendar = {
            era() {
                return "foo";
            },
        };
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7, calendar);
        expect(plainYearMonth.era).toBe("foo");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainYearMonth.prototype, "era", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
