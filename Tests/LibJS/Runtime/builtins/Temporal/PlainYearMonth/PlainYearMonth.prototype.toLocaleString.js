describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainYearMonth.prototype.toLocaleString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainYearMonth;

        plainYearMonth = new Temporal.PlainYearMonth(2021, 7, "gregory");
        expect(plainYearMonth.toLocaleString()).toBe("7/2021");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.toLocaleString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
