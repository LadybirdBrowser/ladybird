describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainMonthDay.from).toHaveLength(1);
    });

    test("fields object argument", () => {
        const object = {
            month: 7,
            day: 6,
        };
        const plainMonthDay = Temporal.PlainMonthDay.from(object);
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);
    });

    for (const [year, februaryDays] of [
        [-1000000, 29],
        [-999999, 28],
        [-1000000000000005, 28],
        [-1000000000000004, 29],
        [1000000000000004, 29],
        [1000000000000005, 28],
        [1000000000000100, 28],
        [1000000000000400, 29],
    ]) {
        test(`ISO year ${year} is only used to apply overflow`, () => {
            for (const overflow of [undefined, "constrain", "reject"]) {
                const january = Temporal.PlainMonthDay.from({ year, month: 1, day: 1 }, { overflow });
                expect(january.toString({ calendarName: "always" })).toBe("1972-01-01[u-ca=iso8601]");

                for (const month of [{ month: 2 }, { monthCode: "M02" }]) {
                    const fields = { year, ...month, day: 29 };
                    if (februaryDays === 28 && overflow === "reject") {
                        expect(() => {
                            Temporal.PlainMonthDay.from(fields, { overflow });
                        }).toThrowWithMessage(RangeError, "Invalid ISO date");
                    } else {
                        const february = Temporal.PlainMonthDay.from(fields, { overflow });
                        expect(february.monthCode).toBe("M02");
                        expect(february.day).toBe(februaryDays);
                        expect(february.toString({ calendarName: "always" })).toBe(
                            `1972-02-${februaryDays}[u-ca=iso8601]`
                        );
                    }
                }
            }
        });
    }

    test("from month day string", () => {
        const plainMonthDay = Temporal.PlainMonthDay.from("--07-06");
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);
    });

    test("from date time string", () => {
        const plainMonthDay = Temporal.PlainMonthDay.from("2021-07-06T23:42:01");
        expect(plainMonthDay.monthCode).toBe("M07");
        expect(plainMonthDay.day).toBe(6);
    });

    test("compares calendar name in month day string in lowercase", () => {
        const values = [
            "02-10[u-ca=iso8601]",
            "02-10[u-ca=isO8601]",
            "02-10[u-ca=iSo8601]",
            "02-10[u-ca=iSO8601]",
            "02-10[u-ca=Iso8601]",
            "02-10[u-ca=IsO8601]",
            "02-10[u-ca=ISo8601]",
            "02-10[u-ca=ISO8601]",
        ];

        for (const value of values) {
            expect(() => {
                Temporal.PlainMonthDay.from(value);
            }).not.toThrowWithMessage(RangeError, "MM-DD string format can only be used with the iso8601 calendar");
        }
    });
});

describe("errors", () => {
    test("missing fields", () => {
        expect(() => {
            Temporal.PlainMonthDay.from({});
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
        expect(() => {
            Temporal.PlainMonthDay.from({ month: 1 });
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
        expect(() => {
            Temporal.PlainMonthDay.from({ day: 1 });
        }).toThrowWithMessage(TypeError, "Required property month is missing or undefined");
    });

    test("invalid month day string", () => {
        expect(() => {
            Temporal.PlainMonthDay.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("string must not contain a UTC designator", () => {
        expect(() => {
            Temporal.PlainMonthDay.from("2021-07-06T23:42:01Z");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("extended year must not be negative zero", () => {
        expect(() => {
            Temporal.PlainMonthDay.from("-000000-01-01");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("can only use iso8601 calendar with month day strings", () => {
        expect(() => {
            Temporal.PlainMonthDay.from("02-10[u-ca=iso8602]");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'iso8602'");

        expect(() => {
            Temporal.PlainMonthDay.from("02-10[u-ca=ladybird]");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'ladybird'");
    });

    test("doesn't throw non-iso8601 calendar error when using a superset format string such as DateTime", () => {
        // NOTE: This will still throw, but only because "ladybird" is not a recognised calendar, not because of the string format restriction.
        try {
            Temporal.PlainMonthDay.from("2023-02-10T22:56[u-ca=ladybird]");
        } catch (e) {
            expect(e).toBeInstanceOf(RangeError);
            expect(e.message).not.toBe("MM-DD string format can only be used with the iso8601 calendar");
            expect(e.message).toBe("Invalid calendar identifier 'ladybird'");
        }
    });
});
