describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDate.prototype.until).toHaveLength(1);
    });

    test("basic functionality", () => {
        const dateOne = new Temporal.PlainDate(2021, 11, 14);
        const dateTwo = new Temporal.PlainDate(2022, 12, 25);
        const untilDuration = dateOne.until(dateTwo);

        expect(untilDuration.years).toBe(0);
        expect(untilDuration.months).toBe(0);
        expect(untilDuration.weeks).toBe(0);
        expect(untilDuration.days).toBe(406);
        expect(untilDuration.hours).toBe(0);
        expect(untilDuration.minutes).toBe(0);
        expect(untilDuration.seconds).toBe(0);
        expect(untilDuration.milliseconds).toBe(0);
        expect(untilDuration.microseconds).toBe(0);
        expect(untilDuration.nanoseconds).toBe(0);
    });

    test("equal dates", () => {
        const equalDateOne = new Temporal.PlainDate(1, 1, 1);
        const equalDateTwo = new Temporal.PlainDate(1, 1, 1);

        const checkResults = result => {
            expect(result.years).toBe(0);
            expect(result.months).toBe(0);
            expect(result.weeks).toBe(0);
            expect(result.days).toBe(0);
            expect(result.hours).toBe(0);
            expect(result.minutes).toBe(0);
            expect(result.seconds).toBe(0);
            expect(result.milliseconds).toBe(0);
            expect(result.microseconds).toBe(0);
            expect(result.nanoseconds).toBe(0);
        };

        checkResults(equalDateOne.until(equalDateOne));
        checkResults(equalDateTwo.until(equalDateTwo));
        checkResults(equalDateOne.until(equalDateTwo));
        checkResults(equalDateTwo.until(equalDateOne));
    });

    test("negative direction", () => {
        const dateOne = new Temporal.PlainDate(2021, 11, 14);
        const dateTwo = new Temporal.PlainDate(2022, 12, 25);
        const untilDuration = dateTwo.until(dateOne);

        expect(untilDuration.years).toBe(0);
        expect(untilDuration.months).toBe(0);
        expect(untilDuration.weeks).toBe(0);
        expect(untilDuration.days).toBe(-406);
        expect(untilDuration.hours).toBe(0);
        expect(untilDuration.minutes).toBe(0);
        expect(untilDuration.seconds).toBe(0);
        expect(untilDuration.milliseconds).toBe(0);
        expect(untilDuration.microseconds).toBe(0);
        expect(untilDuration.nanoseconds).toBe(0);
    });

    test("largestUnit option", () => {
        const values = [
            ["year", { years: 1, months: 1, days: 11 }],
            ["month", { months: 13, days: 11 }],
            ["week", { weeks: 58 }],
            ["day", { days: 406 }],
        ];

        const dateOne = new Temporal.PlainDate(2021, 11, 14);
        const dateTwo = new Temporal.PlainDate(2022, 12, 25);

        for (const [largestUnit, durationLike] of values) {
            const singularOptions = { largestUnit };
            const pluralOptions = { largestUnit: `${largestUnit}s` };

            const propertiesToCheck = Object.keys(durationLike);

            // Positive direction
            const positiveSingularResult = dateOne.until(dateTwo, singularOptions);
            for (const property of propertiesToCheck)
                expect(positiveSingularResult[property]).toBe(durationLike[property]);

            const positivePluralResult = dateOne.until(dateTwo, pluralOptions);
            for (const property of propertiesToCheck)
                expect(positivePluralResult[property]).toBe(durationLike[property]);

            // Negative direction
            const negativeSingularResult = dateTwo.until(dateOne, singularOptions);
            for (const property of propertiesToCheck)
                expect(negativeSingularResult[property]).toBe(-durationLike[property]);

            const negativePluralResult = dateTwo.until(dateOne, pluralOptions);
            for (const property of propertiesToCheck)
                expect(negativePluralResult[property]).toBe(-durationLike[property]);
        }
    });

    test("PlainDate string argument", () => {
        const dateOne = new Temporal.PlainDate(2021, 11, 14);
        const untilDuration = dateOne.until("2022-12-25");

        expect(untilDuration.years).toBe(0);
        expect(untilDuration.months).toBe(0);
        expect(untilDuration.weeks).toBe(0);
        expect(untilDuration.days).toBe(406);
        expect(untilDuration.hours).toBe(0);
        expect(untilDuration.minutes).toBe(0);
        expect(untilDuration.seconds).toBe(0);
        expect(untilDuration.milliseconds).toBe(0);
        expect(untilDuration.microseconds).toBe(0);
        expect(untilDuration.nanoseconds).toBe(0);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Temporal.PlainDate.prototype.until.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
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

        const dateOneWithCalendar = new Temporal.PlainDate(2021, 11, 14, calendarOne);
        const dateTwoWithCalendar = new Temporal.PlainDate(2022, 12, 25, calendarTwo);

        expect(() => {
            dateOneWithCalendar.until(dateTwoWithCalendar);
        }).toThrowWithMessage(RangeError, "Cannot compare dates from two different calendars");
    });

    test("disallowed units", () => {
        const dateOne = new Temporal.PlainDate(2021, 11, 14);
        const dateTwo = new Temporal.PlainDate(2022, 12, 25);

        const disallowedUnits = [
            "hour",
            "minute",
            "second",
            "millisecond",
            "microsecond",
            "nanosecond",
        ];

        for (const smallestUnit of disallowedUnits) {
            const singularSmallestUnitOptions = { smallestUnit };
            const pluralSmallestUnitOptions = { smallestUnit: `${smallestUnit}s` };

            expect(() => {
                dateOne.until(dateTwo, singularSmallestUnitOptions);
            }).toThrowWithMessage(
                RangeError,
                `${smallestUnit} is not a valid value for option smallestUnit`
            );

            expect(() => {
                dateOne.until(dateTwo, pluralSmallestUnitOptions);
            }).toThrowWithMessage(
                RangeError,
                `${smallestUnit}s is not a valid value for option smallestUnit`
            );
        }

        for (const largestUnit of disallowedUnits) {
            const singularLargestUnitOptions = { largestUnit };
            const pluralLargestUnitOptions = { largestUnit: `${largestUnit}s` };

            expect(() => {
                dateOne.until(dateTwo, singularLargestUnitOptions);
            }).toThrowWithMessage(
                RangeError,
                `${largestUnit} is not a valid value for option largestUnit`
            );

            expect(() => {
                dateOne.until(dateTwo, pluralLargestUnitOptions);
            }).toThrowWithMessage(
                RangeError,
                `${largestUnit}s is not a valid value for option largestUnit`
            );
        }
    });

    test("invalid unit range", () => {
        // smallestUnit -> disallowed largestUnits, see validate_temporal_unit_range
        // Note that all the "smallestUnits" are all the allowedUnits.
        const invalidRanges = [
            ["year", ["month", "week", "day"]],
            ["month", ["week", "day"]],
            ["week", ["day"]],
        ];

        const dateOne = new Temporal.PlainDate(2021, 11, 14);
        const dateTwo = new Temporal.PlainDate(2022, 12, 25);

        for (const [smallestUnit, disallowedLargestUnits] of invalidRanges) {
            const pluralSmallestUnit = `${smallestUnit}s`;

            for (const disallowedUnit of disallowedLargestUnits) {
                const pluralDisallowedUnit = `${disallowedUnit}s`;

                const singularSmallestSingularDisallowedOptions = {
                    smallestUnit,
                    largestUnit: disallowedUnit,
                };
                const singularSmallestPluralDisallowedOptions = {
                    smallestUnit,
                    largestUnit: pluralDisallowedUnit,
                };
                const pluralSmallestSingularDisallowedOptions = {
                    smallestUnit: pluralSmallestUnit,
                    largestUnit: disallowedUnit,
                };
                const pluralSmallestPluralDisallowedOptions = {
                    smallestUnit: pluralSmallestUnit,
                    largestUnit: pluralDisallowedUnit,
                };

                expect(() => {
                    dateOne.until(dateTwo, singularSmallestSingularDisallowedOptions);
                }).toThrowWithMessage(
                    RangeError,
                    `Invalid unit range, ${smallestUnit} is larger than ${disallowedUnit}`
                );

                expect(() => {
                    dateOne.until(dateTwo, singularSmallestPluralDisallowedOptions);
                }).toThrowWithMessage(
                    RangeError,
                    `Invalid unit range, ${smallestUnit} is larger than ${disallowedUnit}`
                );

                expect(() => {
                    dateOne.until(dateTwo, pluralSmallestSingularDisallowedOptions);
                }).toThrowWithMessage(
                    RangeError,
                    `Invalid unit range, ${smallestUnit} is larger than ${disallowedUnit}`
                );

                expect(() => {
                    dateOne.until(dateTwo, pluralSmallestPluralDisallowedOptions);
                }).toThrowWithMessage(
                    RangeError,
                    `Invalid unit range, ${smallestUnit} is larger than ${disallowedUnit}`
                );
            }
        }
    });
});
