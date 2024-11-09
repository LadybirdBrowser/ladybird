describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainYearMonth.prototype.since).toHaveLength(1);
    });

    test("basic functionality", () => {
        const values = [
            [[0, 1], [0, 1], "PT0S"],
            [[2, 3], [1, 2], "P1Y1M"],
            [[1, 2], [0, 1], "P1Y1M"],
            [[0, 1], [1, 2], "-P1Y1M"],
            [[0, 12], [0, 1], "P11M"],
            [[0, 1], [0, 12], "-P11M"],
        ];
        for (const [args, argsOther, expected] of values) {
            const plainYearMonth = new Temporal.PlainYearMonth(...args);
            const other = new Temporal.PlainYearMonth(...argsOther);
            expect(plainYearMonth.since(other).toString()).toBe(expected);
        }
    });

    test("smallestUnit option", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(1, 2);
        const other = new Temporal.PlainYearMonth(0, 1);
        const values = [
            ["year", "P1Y"],
            ["month", "P1Y1M"],
        ];
        for (const [smallestUnit, expected] of values) {
            expect(plainYearMonth.since(other, { smallestUnit }).toString()).toBe(expected);
        }
    });

    test("largestUnit option", () => {
        const plainYearMonth = new Temporal.PlainYearMonth(1, 2);
        const other = new Temporal.PlainYearMonth(0, 1);
        const values = [
            ["year", "P1Y1M"],
            ["month", "P13M"],
        ];
        for (const [largestUnit, expected] of values) {
            expect(plainYearMonth.since(other, { largestUnit }).toString()).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.since.call("foo", {});
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });

    test("disallowed smallestUnit option values", () => {
        const values = [
            "week",
            "day",
            "hour",
            "minute",
            "second",
            "millisecond",
            "microsecond",
            "nanosecond",
        ];
        for (const smallestUnit of values) {
            const plainYearMonth = new Temporal.PlainYearMonth(1970, 1);
            const other = new Temporal.PlainYearMonth(1970, 1);
            expect(() => {
                plainYearMonth.since(other, { smallestUnit });
            }).toThrowWithMessage(
                RangeError,
                `${smallestUnit} is not a valid value for option smallestUnit`
            );
        }
    });

    test("disallowed largestUnit option values", () => {
        const values = [
            "week",
            "day",
            "hour",
            "minute",
            "second",
            "millisecond",
            "microsecond",
            "nanosecond",
        ];
        for (const largestUnit of values) {
            const plainYearMonth = new Temporal.PlainYearMonth(1970, 1);
            const other = new Temporal.PlainYearMonth(1970, 1);
            expect(() => {
                plainYearMonth.since(other, { largestUnit });
            }).toThrowWithMessage(
                RangeError,
                `${largestUnit} is not a valid value for option largestUnit`
            );
        }
    });

    test("cannot compare dates from different calendars", () => {
        const calendarOne = {
            toString() {
                return "calendarOne";
            },
        };

        const calendarTwo = {
            toString() {
                return "calendarTwo";
            },
        };

        const plainYearMonthOne = new Temporal.PlainYearMonth(1970, 1, calendarOne);
        const plainYearMonthTwo = new Temporal.PlainYearMonth(1970, 1, calendarTwo);

        expect(() => {
            plainYearMonthOne.since(plainYearMonthTwo);
        }).toThrowWithMessage(RangeError, "Cannot compare dates from two different calendars");
    });
});
