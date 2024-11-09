describe("correct behavior", () => {
    test("basic functionality", () => {
        {
            const duration = new Temporal.Duration(0, 0, 0, 0, 1, 2, 3, 4, 5, 6);
            const relativeTo = new Temporal.PlainDate(1970, 1, 1);
            const values = [
                [{ unit: "year", relativeTo }, 0.0001180556825534627],
                [{ unit: "month", relativeTo }, 0.0013900104558714158],
                [{ unit: "week", relativeTo }, 0.006155760590287699],
                [{ unit: "day", relativeTo }, 0.04309032413201389],
                [{ unit: "hour" }, 1.034167779168333],
                [{ unit: "minute" }, 62.0500667501],
                [{ unit: "second" }, 3723.00400500600017],
                [{ unit: "millisecond" }, 3723004.005005999933928],
                [{ unit: "microsecond" }, 3723004005.006000041961669],
                [{ unit: "nanosecond" }, 3723004005006],
            ];
            for (const [arg, expected] of values) {
                const matcher = Number.isInteger(expected) ? "toBe" : "toBeCloseTo";
                expect(duration.total(arg))[matcher](expected);
            }
        }

        {
            const duration = new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
            const relativeTo = new Temporal.PlainDate(1970, 1, 1);
            const values = [
                [{ unit: "year", relativeTo }, 1.2307194003046997],
                [{ unit: "month", relativeTo }, 14.813309068103722],
                [{ unit: "week", relativeTo }, 64.17322587303077],
                [{ unit: "day", relativeTo }, 449.21258111121534],
                [{ unit: "hour", relativeTo }, 10781.101946669169],
                [{ unit: "minute", relativeTo }, 646866.1168001501],
                [{ unit: "second", relativeTo }, 38811967.00800901],
                [{ unit: "millisecond", relativeTo }, 38811967008.00901],
                [{ unit: "microsecond", relativeTo }, 38811967008009.01],
                [{ unit: "nanosecond", relativeTo }, 38811967008009010],
            ];
            for (const [arg, expected] of values) {
                const matcher = Number.isInteger(expected) ? "toBe" : "toBeCloseTo";
                expect(duration.total(arg))[matcher](expected);
            }
        }

        {
            const relativeTo = new Temporal.PlainDate(1970, 1, 1);
            const units = [
                "year",
                "month",
                "week",
                "day",
                "hour",
                "minute",
                "second",
                "millisecond",
                "microsecond",
                "nanosecond",
            ];
            for (let i = 0; i < 10; ++i) {
                const args = [0, 0, 0, 0, 0, 0, 0, 0, 0];
                args[i] = 123;
                const unit = units[i];
                const duration = new Temporal.Duration(...args);
                expect(duration.total({ unit, relativeTo })).toBe(123);
            }
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.total.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });

    test("missing options object", () => {
        const duration = new Temporal.Duration();
        expect(() => {
            duration.total();
        }).toThrowWithMessage(TypeError, "Required options object is missing or undefined");
    });

    test("missing unit option", () => {
        const duration = new Temporal.Duration();
        expect(() => {
            duration.total({});
        }).toThrowWithMessage(RangeError, "unit option value is undefined");
    });

    test("invalid unit option", () => {
        const duration = new Temporal.Duration();
        expect(() => {
            duration.total({ unit: "foo" });
        }).toThrowWithMessage(RangeError, "foo is not a valid value for option unit");
    });

    test("relativeTo is required when duration has calendar units", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.total({ unit: "second" });
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing calendar units"
        );
    });
});
