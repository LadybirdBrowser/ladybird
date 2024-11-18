describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Duration.prototype.toString).toHaveLength(0);
    });

    test("basic functionality", () => {
        const values = [
            [[0], "PT0S"],
            [[1], "P1Y"],
            [[0, 1], "P1M"],
            [[0, 0, 1], "P1W"],
            [[0, 0, 0, 1], "P1D"],
            [[0, 0, 0, 0, 1], "PT1H"],
            [[0, 0, 0, 0, 0, 1], "PT1M"],
            [[0, 0, 0, 0, 0, 0, 1], "PT1S"],
            [[0, 0, 0, 0, 0, 0, 0, 1], "PT0.001S"],
            [[0, 0, 0, 0, 0, 0, 0, 0, 1], "PT0.000001S"],
            [[0, 0, 0, 0, 0, 0, 0, 0, 0, 1], "PT0.000000001S"],
            [[1, 2], "P1Y2M"],
            [[1, 2, 3], "P1Y2M3W"],
            [[1, 2, 3, 4], "P1Y2M3W4D"],
            [[1, 2, 3, 4, 5], "P1Y2M3W4DT5H"],
            [[1, 2, 3, 4, 5, 6], "P1Y2M3W4DT5H6M"],
            [[1, 2, 3, 4, 5, 6, 7], "P1Y2M3W4DT5H6M7S"],
            [[1, 2, 3, 4, 5, 6, 7, 8], "P1Y2M3W4DT5H6M7.008S"],
            [[1, 2, 3, 4, 5, 6, 7, 8, 9], "P1Y2M3W4DT5H6M7.008009S"],
            [[1, 2, 3, 4, 5, 6, 7, 8, 9, 10], "P1Y2M3W4DT5H6M7.00800901S"],
            [
                [100, 200, 300, 400, 500, 600, 700, 800, 900, 1000],
                "P100Y200M300W400DT500H600M700.800901S",
            ],
            [[-1], "-P1Y"],
        ];
        for (const [args, expected] of values) {
            expect(new Temporal.Duration(...args).toString()).toBe(expected);
        }
    });

    test("smallestUnit option", () => {
        const values = [
            ["second", "P1Y2M3W4DT5H6M7S"],
            ["millisecond", "P1Y2M3W4DT5H6M7.008S"],
            ["microsecond", "P1Y2M3W4DT5H6M7.008010S"],
            ["nanosecond", "P1Y2M3W4DT5H6M7.008010000S"],
        ];
        for (const [smallestUnit, expected] of values) {
            expect(
                new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 10).toString({ smallestUnit })
            ).toBe(expected);
        }
    });

    test("fractionalSecondDigits option", () => {
        const values = [
            [0, "P1Y2M3W4DT5H6M7S"],
            [1, "P1Y2M3W4DT5H6M7.0S"],
            [2, "P1Y2M3W4DT5H6M7.00S"],
            [3, "P1Y2M3W4DT5H6M7.008S"],
            [4, "P1Y2M3W4DT5H6M7.0080S"],
            [5, "P1Y2M3W4DT5H6M7.00801S"],
            [6, "P1Y2M3W4DT5H6M7.008010S"],
            [7, "P1Y2M3W4DT5H6M7.0080100S"],
            [8, "P1Y2M3W4DT5H6M7.00801000S"],
            [9, "P1Y2M3W4DT5H6M7.008010000S"],
        ];
        for (const [fractionalSecondDigits, expected] of values) {
            expect(
                new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 10).toString({
                    fractionalSecondDigits,
                })
            ).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.toString.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });

    test("disallowed smallestUnit option values", () => {
        const values = ["year", "month", "week", "day", "hour", "minute"];
        for (const smallestUnit of values) {
            expect(() => {
                new Temporal.Duration(0).toString({ smallestUnit });
            }).toThrowWithMessage(
                RangeError,
                `${smallestUnit} is not a valid value for option smallestUnit`
            );
        }
    });
});
