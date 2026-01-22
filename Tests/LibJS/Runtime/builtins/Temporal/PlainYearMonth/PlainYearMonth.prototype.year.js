describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.year).toBe(2021);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainYearMonth.prototype, "year", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
