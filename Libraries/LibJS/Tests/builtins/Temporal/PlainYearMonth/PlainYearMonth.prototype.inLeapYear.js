describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.inLeapYear).toBe(false);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainYearMonth.prototype, "inLeapYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
