describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.since).toHaveLength(1);
    });

    test("basic functionality", () => {
        const values = [
            [[0, 1, 1, 0, 0, 0, 0, 0, 0], [0, 1, 1, 0, 0, 0, 0, 0, 0], "PT0S"],
            [[2, 3, 4, 5, 6, 7, 8, 9, 10], [1, 2, 3, 4, 5, 6, 7, 8, 9], "P394DT1H1M1.001001001S"],
            [[1, 2, 3, 4, 5, 6, 7, 8, 9], [0, 1, 1, 0, 0, 0, 0, 0, 0], "P399DT4H5M6.007008009S"],
            [[0, 1, 1, 0, 0, 0, 0, 0, 0], [1, 2, 3, 4, 5, 6, 7, 8, 9], "-P399DT4H5M6.007008009S"],
            [
                [0, 12, 31, 23, 59, 59, 999, 999, 999],
                [0, 1, 1, 0, 0, 0, 0, 0, 0],
                "P365DT23H59M59.999999999S",
            ],
            [
                [0, 1, 1, 0, 0, 0, 0, 0, 0],
                [0, 12, 31, 23, 59, 59, 999, 999, 999],
                "-P365DT23H59M59.999999999S",
            ],
        ];
        for (const [args, argsOther, expected] of values) {
            const plainDateTime = new Temporal.PlainDateTime(...args);
            const other = new Temporal.PlainDateTime(...argsOther);
            expect(plainDateTime.since(other).toString()).toBe(expected);
        }
    });

    test("smallestUnit option", () => {
        const plainDateTime = new Temporal.PlainDateTime(1, 2, 3, 4, 5, 6, 7, 8, 9);
        const other = new Temporal.PlainDateTime(0, 1, 1, 0, 0, 0, 0, 0, 0);
        const values = [
            ["year", "P1Y"],
            ["month", "P13M"],
            ["week", "P57W"],
            ["day", "P399D"],
            ["hour", "P399DT4H"],
            ["minute", "P399DT4H5M"],
            ["second", "P399DT4H5M6S"],
            ["millisecond", "P399DT4H5M6.007S"],
            ["microsecond", "P399DT4H5M6.007008S"],
            ["nanosecond", "P399DT4H5M6.007008009S"],
        ];
        for (const [smallestUnit, expected] of values) {
            expect(plainDateTime.since(other, { smallestUnit }).toString()).toBe(expected);
        }
    });

    test("largestUnit option", () => {
        const plainDateTime = new Temporal.PlainDateTime(1, 2, 3, 4, 5, 6, 7, 8, 9);
        const other = new Temporal.PlainDateTime(0, 1, 1, 0, 0, 0, 0, 0, 0);
        const values = [
            ["year", "P1Y1M2DT4H5M6.007008009S"],
            ["month", "P13M2DT4H5M6.007008009S"],
            ["week", "P57WT4H5M6.007008009S"],
            ["day", "P399DT4H5M6.007008009S"],
            ["hour", "PT9580H5M6.007008009S"],
            ["minute", "PT574805M6.007008009S"],
            ["second", "PT34488306.007008009S"],
            ["millisecond", "PT34488306.007008009S"],
            ["microsecond", "PT34488306.007008009S"],
            ["nanosecond", "PT34488306.007008008S"],
        ];
        for (const [largestUnit, expected] of values) {
            expect(plainDateTime.since(other, { largestUnit }).toString()).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.since.call("foo", {});
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
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

        const args = [1970, 1, 1, 0, 0, 0, 0, 0, 0];
        const plainDateTimeOne = new Temporal.PlainDateTime(...args, calendarOne);
        const plainDateTimeTwo = new Temporal.PlainDateTime(...args, calendarTwo);

        expect(() => {
            plainDateTimeOne.since(plainDateTimeTwo);
        }).toThrowWithMessage(RangeError, "Cannot compare dates from two different calendars");
    });
});
