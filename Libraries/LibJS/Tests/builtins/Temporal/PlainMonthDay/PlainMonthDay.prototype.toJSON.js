describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainMonthDay.prototype.toJSON).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainMonthDay;

        plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(plainMonthDay.toJSON()).toBe("07-06");

        plainMonthDay = new Temporal.PlainMonthDay(7, 6, { toString: () => "foo" }, 2021);
        expect(plainMonthDay.toJSON()).toBe("2021-07-06[u-ca=foo]");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Temporal.PlainMonthDay.prototype.toJSON.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });
});
