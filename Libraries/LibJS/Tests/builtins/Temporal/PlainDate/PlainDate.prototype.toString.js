describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDate.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        let plainDate;

        plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(plainDate.toString()).toBe("2021-07-06");
        expect(plainDate.toString({ calendarName: "auto" })).toBe("2021-07-06");
        expect(plainDate.toString({ calendarName: "always" })).toBe("2021-07-06[u-ca=iso8601]");
        expect(plainDate.toString({ calendarName: "never" })).toBe("2021-07-06");
        expect(plainDate.toString({ calendarName: "critical" })).toBe("2021-07-06[!u-ca=iso8601]");

        plainDate = new Temporal.PlainDate(2021, 7, 6, { toString: () => "foo" });
        expect(plainDate.toString()).toBe("2021-07-06[u-ca=foo]");
        expect(plainDate.toString({ calendarName: "auto" })).toBe("2021-07-06[u-ca=foo]");
        expect(plainDate.toString({ calendarName: "always" })).toBe("2021-07-06[u-ca=foo]");
        expect(plainDate.toString({ calendarName: "never" })).toBe("2021-07-06");
        expect(plainDate.toString({ calendarName: "critical" })).toBe("2021-07-06[!u-ca=foo]");

        plainDate = new Temporal.PlainDate(0, 1, 1);
        expect(plainDate.toString()).toBe("0000-01-01");

        plainDate = new Temporal.PlainDate(999, 1, 1);
        expect(plainDate.toString()).toBe("0999-01-01");

        plainDate = new Temporal.PlainDate(9999, 1, 1);
        expect(plainDate.toString()).toBe("9999-01-01");

        plainDate = new Temporal.PlainDate(12345, 1, 1);
        expect(plainDate.toString()).toBe("+012345-01-01");

        plainDate = new Temporal.PlainDate(123456, 1, 1);
        expect(plainDate.toString()).toBe("+123456-01-01");

        plainDate = new Temporal.PlainDate(-12345, 1, 1);
        expect(plainDate.toString()).toBe("-012345-01-01");
    });

    test("doesn't call ToString on calendar if calenderName option is 'never'", () => {
        let calledToString = false;
        const calendar = {
            toString() {
                calledToString = true;
                return "nocall";
            },
        };

        const plainDate = new Temporal.PlainDate(2022, 8, 8, calendar);
        const options = {
            calendarName: "never",
        };
        expect(plainDate.toString(options)).toBe("2022-08-08");
        expect(calledToString).toBeFalse();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });

    test("calendarName option must be one of 'auto', 'always', 'never', 'critical'", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        expect(() => {
            plainDate.toString({ calendarName: "foo" });
        }).toThrowWithMessage(RangeError, "foo is not a valid value for option calendarName");
    });
});
