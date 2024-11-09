describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Duration.prototype.round).toHaveLength(1);
    });

    test("basic functionality", () => {
        const duration = new Temporal.Duration(0, 0, 0, 21, 7, 10, 100, 200, 300, 400);
        const values = [
            ["nanosecond", "P21DT7H11M40.2003004S"],
            ["microsecond", "P21DT7H11M40.2003S"],
            ["millisecond", "P21DT7H11M40.2S"],
            ["second", "P21DT7H11M40S"],
            ["minute", "P21DT7H12M"],
            ["hour", "P21DT7H"],
            ["day", "P21D"],
        ];

        for (const [smallestUnit, durationString] of values) {
            const singularRoundedDuration = duration.round({ smallestUnit });
            const pluralRoundedDuration = duration.round({ smallestUnit: `${smallestUnit}s` });

            // Passing in a string is treated as though { smallestUnit: "<string value>" } was passed in.
            const singularRoundedDurationWithString = duration.round(smallestUnit);
            const pluralRoundedDurationWithString = duration.round(`${smallestUnit}s`);

            expect(singularRoundedDuration.toString()).toBe(durationString);
            expect(singularRoundedDurationWithString.toString()).toBe(durationString);
            expect(pluralRoundedDuration.toString()).toBe(durationString);
            expect(pluralRoundedDurationWithString.toString()).toBe(durationString);
        }
    });

    test("largestUnit option", () => {
        const duration = new Temporal.Duration(0, 0, 0, 21, 7, 10, 100, 200, 300, 400);

        // Using strings is not sufficient here, for example, the nanosecond case will produce "PT1840300.2003004S" which is 1840300 s, 200 ms, 300 us, 400 ns
        const values = [
            ["nanosecond", { nanoseconds: 1840300200300400 }],
            ["microsecond", { microseconds: 1840300200300, nanoseconds: 400 }],
            ["millisecond", { milliseconds: 1840300200, microseconds: 300, nanoseconds: 400 }],
            [
                "second",
                { seconds: 1840300, milliseconds: 200, microseconds: 300, nanoseconds: 400 },
            ],
            [
                "minute",
                {
                    minutes: 30671,
                    seconds: 40,
                    milliseconds: 200,
                    microseconds: 300,
                    nanoseconds: 400,
                },
            ],
            [
                "hour",
                {
                    hours: 511,
                    minutes: 11,
                    seconds: 40,
                    milliseconds: 200,
                    microseconds: 300,
                    nanoseconds: 400,
                },
            ],
            [
                "day",
                {
                    days: 21,
                    hours: 7,
                    minutes: 11,
                    seconds: 40,
                    milliseconds: 200,
                    microseconds: 300,
                    nanoseconds: 400,
                },
            ],
        ];

        for (const [largestUnit, durationLike] of values) {
            const singularRoundedDuration = duration.round({ largestUnit });
            const pluralRoundedDuration = duration.round({ largestUnit: `${largestUnit}s` });

            const propertiesToCheck = Object.keys(durationLike);

            for (const property of propertiesToCheck) {
                expect(singularRoundedDuration[property]).toBe(durationLike[property]);
                expect(pluralRoundedDuration[property]).toBe(durationLike[property]);
            }
        }
    });
});

describe("errors", () => {
    test("this value must be a Temporal.Duration object", () => {
        expect(() => {
            Temporal.Duration.prototype.round.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.Duration");
    });

    test("missing options object", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round();
        }).toThrowWithMessage(TypeError, "Required options object is missing or undefined");
    });

    test("invalid rounding mode", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({ smallestUnit: "second", roundingMode: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option roundingMode"
        );
    });

    test("invalid smallest unit", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({ smallestUnit: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option smallestUnit"
        );
    });

    test("increment may not be NaN", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({ smallestUnit: "second", roundingIncrement: NaN });
        }).toThrowWithMessage(RangeError, "NaN is not a valid value for option roundingIncrement");
    });

    test("increment may smaller than 1 or larger than maximum", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({ smallestUnit: "second", roundingIncrement: -1 });
        }).toThrowWithMessage(RangeError, "-1 is not a valid value for option roundingIncrement");
        expect(() => {
            duration.round({ smallestUnit: "second", roundingIncrement: 0 });
        }).toThrowWithMessage(RangeError, "0 is not a valid value for option roundingIncrement");
        expect(() => {
            duration.round({ smallestUnit: "second", roundingIncrement: Infinity });
        }).toThrowWithMessage(RangeError, "inf is not a valid value for option roundingIncrement");
    });

    test("must provide one or both of smallestUnit or largestUnit", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({});
        }).toThrowWithMessage(RangeError, "One or both of smallestUnit or largestUnit is required");
    });

    test("relativeTo is required when duration has calendar units", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({ largestUnit: "second" });
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing calendar units"
        );
    });

    // Spec Issue: https://github.com/tc39/proposal-temporal/issues/2124
    // Spec Fix: https://github.com/tc39/proposal-temporal/commit/66f7464aaec64d3cd21fb2ec37f6502743b9a730
    test("balancing calendar units with largestUnit set to 'year' and relativeTo unset throws instead of crashing", () => {
        const duration = new Temporal.Duration(1);
        expect(() => {
            duration.round({ largestUnit: "year" });
        }).toThrowWithMessage(
            RangeError,
            "A starting point is required for balancing calendar units"
        );
    });

    test("invalid calendar throws range exception when performing round", () => {
        const duration = Temporal.Duration.from({ nanoseconds: 0 });

        const calendar = new (class extends Temporal.Calendar {
            dateAdd(date, duration, options) {
                return date;
            }
        })("iso8601");

        expect(() => {
            duration.round({
                relativeTo: new Temporal.PlainDate(1997, 5, 10, calendar),
                smallestUnit: "years",
            });
        }).toThrowWithMessage(
            RangeError,
            "Invalid calendar, dateAdd() function returned result implying a year is zero days long"
        );
    });
});
