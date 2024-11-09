describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.until).toHaveLength(1);
    });

    test("basic functionality", () => {
        const values = [
            [[0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0], "PT0S"],
            [[1, 2, 3, 4, 5, 6], [2, 3, 4, 5, 6, 7], "PT1H1M1.001001001S"],
            [[0, 0, 0, 0, 0, 0], [1, 2, 3, 4, 5, 6], "PT1H2M3.004005006S"],
            [[1, 2, 3, 4, 5, 6], [0, 0, 0, 0, 0, 0], "-PT1H2M3.004005006S"],
            [[0, 0, 0, 0, 0, 0], [23, 59, 59, 999, 999, 999], "PT23H59M59.999999999S"],
            [[23, 59, 59, 999, 999, 999], [0, 0, 0, 0, 0, 0], "-PT23H59M59.999999999S"],
        ];
        for (const [args, argsOther, expected] of values) {
            const plainTime = new Temporal.PlainTime(...args);
            const other = new Temporal.PlainTime(...argsOther);
            expect(plainTime.until(other).toString()).toBe(expected);
        }
    });

    test("smallestUnit option", () => {
        const plainTime = new Temporal.PlainTime(0, 0, 0, 0, 0, 0);
        const other = new Temporal.PlainTime(1, 2, 3, 4, 5, 6);
        const values = [
            ["hour", "PT1H"],
            ["minute", "PT1H2M"],
            ["second", "PT1H2M3S"],
            ["millisecond", "PT1H2M3.004S"],
            ["microsecond", "PT1H2M3.004005S"],
            ["nanosecond", "PT1H2M3.004005006S"],
        ];
        for (const [smallestUnit, expected] of values) {
            expect(plainTime.until(other, { smallestUnit }).toString()).toBe(expected);
        }
    });

    test("largestUnit option", () => {
        const plainTime = new Temporal.PlainTime(0, 0, 0, 0, 0, 0);
        const other = new Temporal.PlainTime(1, 2, 3, 4, 5, 6);
        const values = [
            ["hour", "PT1H2M3.004005006S"],
            ["minute", "PT62M3.004005006S"],
            ["second", "PT3723.004005006S"],
            ["millisecond", "PT3723.004005006S"],
            ["microsecond", "PT3723.004005006S"],
            ["nanosecond", "PT3723.004005006S"],
        ];
        for (const [largestUnit, expected] of values) {
            expect(plainTime.until(other, { largestUnit }).toString()).toBe(expected);
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.until.call("foo", {});
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });

    test("disallowed smallestUnit option values", () => {
        const values = ["year", "month", "week", "day"];
        for (const smallestUnit of values) {
            const plainTime = new Temporal.PlainTime();
            const other = new Temporal.PlainTime();
            expect(() => {
                plainTime.until(other, { smallestUnit });
            }).toThrowWithMessage(
                RangeError,
                `${smallestUnit} is not a valid value for option smallestUnit`
            );
        }
    });

    test("disallowed largestUnit option values", () => {
        const values = ["year", "month", "week", "day"];
        for (const largestUnit of values) {
            const plainTime = new Temporal.PlainTime();
            const other = new Temporal.PlainTime();
            expect(() => {
                plainTime.until(other, { largestUnit });
            }).toThrowWithMessage(
                RangeError,
                `${largestUnit} is not a valid value for option largestUnit`
            );
        }
    });
});
