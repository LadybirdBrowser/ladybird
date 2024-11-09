describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainYearMonth.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainYearMonth;

        plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(plainYearMonth.toString()).toBe("2021-07");
        expect(plainYearMonth.toString({ calendarName: "auto" })).toBe("2021-07");
        expect(plainYearMonth.toString({ calendarName: "always" })).toBe(
            "2021-07-01[u-ca=iso8601]"
        );
        expect(plainYearMonth.toString({ calendarName: "never" })).toBe("2021-07");
        expect(plainYearMonth.toString({ calendarName: "critical" })).toBe(
            "2021-07-01[!u-ca=iso8601]"
        );

        plainYearMonth = new Temporal.PlainYearMonth(2021, 7, { toString: () => "foo" }, 6);
        expect(plainYearMonth.toString()).toBe("2021-07-06[u-ca=foo]");
        expect(plainYearMonth.toString({ calendarName: "auto" })).toBe("2021-07-06[u-ca=foo]");
        expect(plainYearMonth.toString({ calendarName: "always" })).toBe("2021-07-06[u-ca=foo]");
        expect(plainYearMonth.toString({ calendarName: "never" })).toBe("2021-07-06");
        expect(plainYearMonth.toString({ calendarName: "critical" })).toBe("2021-07-06[!u-ca=foo]");

        plainYearMonth = new Temporal.PlainYearMonth(0, 1);
        expect(plainYearMonth.toString()).toBe("0000-01");

        plainYearMonth = new Temporal.PlainYearMonth(999, 1);
        expect(plainYearMonth.toString()).toBe("0999-01");

        plainYearMonth = new Temporal.PlainYearMonth(9999, 1);
        expect(plainYearMonth.toString()).toBe("9999-01");

        plainYearMonth = new Temporal.PlainYearMonth(12345, 1);
        expect(plainYearMonth.toString()).toBe("+012345-01");

        plainYearMonth = new Temporal.PlainYearMonth(123456, 1);
        expect(plainYearMonth.toString()).toBe("+123456-01");

        plainYearMonth = new Temporal.PlainYearMonth(-12345, 1);
        expect(plainYearMonth.toString()).toBe("-012345-01");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });

    test("calendarName option must be one of 'auto', 'always', 'never', 'critical'", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7);
        expect(() => {
            plainYearMonth.toString({ calendarName: "foo" });
        }).toThrowWithMessage(RangeError, "foo is not a valid value for option calendarName");
    });
});
