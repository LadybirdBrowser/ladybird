describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainMonthDay.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainMonthDay;

        plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(plainMonthDay.toString()).toBe("07-06");
        expect(plainMonthDay.toString({ calendarName: "auto" })).toBe("07-06");
        expect(plainMonthDay.toString({ calendarName: "always" })).toBe("1972-07-06[u-ca=iso8601]");
        expect(plainMonthDay.toString({ calendarName: "never" })).toBe("07-06");
        expect(plainMonthDay.toString({ calendarName: "critical" })).toBe(
            "1972-07-06[!u-ca=iso8601]"
        );

        plainMonthDay = new Temporal.PlainMonthDay(7, 6, { toString: () => "foo" }, 2021);
        expect(plainMonthDay.toString()).toBe("2021-07-06[u-ca=foo]");
        expect(plainMonthDay.toString({ calendarName: "auto" })).toBe("2021-07-06[u-ca=foo]");
        expect(plainMonthDay.toString({ calendarName: "always" })).toBe("2021-07-06[u-ca=foo]");
        expect(plainMonthDay.toString({ calendarName: "never" })).toBe("2021-07-06");
        expect(plainMonthDay.toString({ calendarName: "critical" })).toBe("2021-07-06[!u-ca=foo]");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainMonthDay object", () => {
        expect(() => {
            Temporal.PlainMonthDay.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainMonthDay");
    });

    test("calendarName option must be one of 'auto', 'always', 'never', 'critical'", () => {
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(() => {
            plainMonthDay.toString({ calendarName: "foo" });
        }).toThrowWithMessage(RangeError, "foo is not a valid value for option calendarName");
    });
});
