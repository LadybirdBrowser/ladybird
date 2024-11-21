describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainYearMonth.prototype.toJSON).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainYearMonth;

        plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.toJSON()).toBe("2021-07");

        plainYearMonth = new Temporal.PlainYearMonth(2021, 7, "gregory", 6);
        expect(plainYearMonth.toJSON()).toBe("2021-07-06[u-ca=gregory]");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.toJSON.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
