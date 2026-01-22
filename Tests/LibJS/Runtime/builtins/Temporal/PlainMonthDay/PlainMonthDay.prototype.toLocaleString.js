describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainMonthDay.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainMonthDay;

        plainMonthDay = new Temporal.PlainMonthDay(7, 6, "gregory");
        expect(plainMonthDay.toLocaleString()).toBe("7/6");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Temporal.PlainMonthDay.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });
});
