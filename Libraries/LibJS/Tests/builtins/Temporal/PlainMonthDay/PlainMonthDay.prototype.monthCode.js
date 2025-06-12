describe("correct behavior", () => {
    test("basic functionality", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(plainMonthDay.monthCode).toBe("M07");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainMonthDay.prototype, "monthCode", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });
});
