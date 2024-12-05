describe("correct behavior", () => {
    test("basic functionality", () => {
        {
            const duration = new Temporal.Duration(0, 0, 0, 0, 1, 2, 3, 4, 5, 6);
            const values = [
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
    });

    test("relative to plain date", () => {
        const duration = new Temporal.Duration(0, 0, 0, 31);

        ["2000-01-01", "2000-01-01T00:00", "2000-01-01T00:00[u-ca=iso8601]"].forEach(relativeTo => {
            const result = duration.total({ unit: "months", relativeTo });
            expect(result).toBe(1);
        });
    });

    test("relative to zoned date time", () => {
        const duration = new Temporal.Duration(0, 0, 0, 31);

        [
            "2000-01-01[UTC]",
            "2000-01-01T00:00[UTC]",
            "2000-01-01T00:00+00:00[UTC]",
            "2000-01-01T00:00+00:00[UTC][u-ca=iso8601]",
        ].forEach(relativeTo => {
            const result = duration.total({ unit: "months", relativeTo });
            expect(result).toBe(1);
        });
    });

    test("match minutes", () => {
        const duration = new Temporal.Duration(1, 0, 0, 0, 24);

        const result = duration.total({
            unit: "days",
            relativeTo: "1970-01-01T00:00:00-00:45[Africa/Monrovia]",
        });
        expect(result).toBe(366);
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
        }).toThrowWithMessage(TypeError, "totalOf is undefined");
    });

    test("missing unit option", () => {
        const duration = new Temporal.Duration();
        expect(() => {
            duration.total({});
        }).toThrowWithMessage(RangeError, "undefined is not a valid value for option unit");
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
        }).toThrowWithMessage(RangeError, "Largest unit must not be year");
    });

    test("relativeTo with invalid date", () => {
        const duration = new Temporal.Duration(0, 0, 0, 31);

        expect(() => {
            duration.total({ unit: "minute", relativeTo: "-271821-04-19" });
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });
});
