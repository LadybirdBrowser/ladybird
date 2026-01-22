describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.daysInYear).toBe(365);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainYearMonth.prototype, "daysInYear", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
