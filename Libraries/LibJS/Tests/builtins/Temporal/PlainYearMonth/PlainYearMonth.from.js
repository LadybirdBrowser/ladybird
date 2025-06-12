describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainYearMonth.from).toHaveLength(1);
    });

    test("PlainYearMonth instance argument", () => {
        const plainYearMonth_ = new Temporal.PlainYearMonth(2021, 7);
        const plainYearMonth = Temporal.PlainYearMonth.from(plainYearMonth_);
        expect(plainYearMonth.year).toBe(2021);
        expect(plainYearMonth.month).toBe(7);
        expect(plainYearMonth.monthCode).toBe("M07");
        expect(plainYearMonth.daysInYear).toBe(365);
        expect(plainYearMonth.daysInMonth).toBe(31);
        expect(plainYearMonth.monthsInYear).toBe(12);
        expect(plainYearMonth.inLeapYear).toBeFalse();
    });

    test("fields object argument", () => {
        const object = {
            year: 2021,
            month: 7,
        };
        const plainYearMonth = Temporal.PlainYearMonth.from(object);
        expect(plainYearMonth.year).toBe(2021);
        expect(plainYearMonth.month).toBe(7);
        expect(plainYearMonth.monthCode).toBe("M07");
        expect(plainYearMonth.daysInYear).toBe(365);
        expect(plainYearMonth.daysInMonth).toBe(31);
        expect(plainYearMonth.monthsInYear).toBe(12);
        expect(plainYearMonth.inLeapYear).toBeFalse();
    });

    test("from year month string", () => {
        const plainYearMonth = Temporal.PlainYearMonth.from("2021-07");
        expect(plainYearMonth.year).toBe(2021);
        expect(plainYearMonth.month).toBe(7);
        expect(plainYearMonth.monthCode).toBe("M07");
    });

    test("from date time string", () => {
        const plainYearMonth = Temporal.PlainYearMonth.from("2021-07-06T23:42:01");
        expect(plainYearMonth.year).toBe(2021);
        expect(plainYearMonth.month).toBe(7);
        expect(plainYearMonth.monthCode).toBe("M07");
        expect(plainYearMonth.daysInYear).toBe(365);
        expect(plainYearMonth.daysInMonth).toBe(31);
        expect(plainYearMonth.monthsInYear).toBe(12);
        expect(plainYearMonth.inLeapYear).toBeFalse();
    });

    test("compares calendar name in year month string in lowercase", () => {
        const values = [
            "2023-02[u-ca=iso8601]",
            "2023-02[u-ca=isO8601]",
            "2023-02[u-ca=iSo8601]",
            "2023-02[u-ca=iSO8601]",
            "2023-02[u-ca=Iso8601]",
            "2023-02[u-ca=IsO8601]",
            "2023-02[u-ca=ISo8601]",
            "2023-02[u-ca=ISO8601]",
        ];

        for (const value of values) {
            expect(() => {
                Temporal.PlainYearMonth.from(value);
            }).not.toThrowWithMessage(
                RangeError,
                "YYYY-MM string format can only be used with the iso8601 calendar"
            );
        }
    });
});

describe("errors", () => {
    test("missing fields", () => {
        expect(() => {
            Temporal.PlainYearMonth.from({});
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
        expect(() => {
            Temporal.PlainYearMonth.from({ year: 0 });
        }).toThrowWithMessage(TypeError, "Required property month is missing or undefined");
        expect(() => {
            Temporal.PlainYearMonth.from({ month: 1 });
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
    });

    test("invalid year month string", () => {
        expect(() => {
            Temporal.PlainYearMonth.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("string must not contain a UTC designator", () => {
        expect(() => {
            Temporal.PlainYearMonth.from("2021-07-06T23:42:01Z");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("extended year must not be negative zero", () => {
        expect(() => {
            Temporal.PlainYearMonth.from("-000000-01");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
        expect(() => {
            Temporal.PlainYearMonth.from("−000000-01"); // U+2212
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("can only use iso8601 calendar with year month strings", () => {
        expect(() => {
            Temporal.PlainYearMonth.from("2023-02[u-ca=iso8602]");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'iso8602'");

        expect(() => {
            Temporal.PlainYearMonth.from("2023-02[u-ca=ladybird]");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'ladybird'");
    });

    test("doesn't throw non-iso8601 calendar error when using a superset format string such as DateTime", () => {
        // NOTE: This will still throw, but only because "ladybird" is not a recognised calendar, not because of the string format restriction.
        try {
            Temporal.PlainYearMonth.from("2023-02-10T22:57[u-ca=ladybird]");
        } catch (e) {
            expect(e).toBeInstanceOf(RangeError);
            expect(e.message).not.toBe(
                "MM-DD string format can only be used with the iso8601 calendar"
            );
            expect(e.message).toBe("Invalid calendar identifier 'ladybird'");
        }
    });
});
