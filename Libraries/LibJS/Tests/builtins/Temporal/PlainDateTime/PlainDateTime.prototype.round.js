describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.round).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);

        const firstRoundedPlainDateTime = plainDateTime.round({ smallestUnit: "minute" });
        expect(firstRoundedPlainDateTime.year).toBe(2021);
        expect(firstRoundedPlainDateTime.month).toBe(11);
        expect(firstRoundedPlainDateTime.monthCode).toBe("M11");
        expect(firstRoundedPlainDateTime.day).toBe(3);
        expect(firstRoundedPlainDateTime.hour).toBe(18);
        expect(firstRoundedPlainDateTime.minute).toBe(8);
        expect(firstRoundedPlainDateTime.second).toBe(0);
        expect(firstRoundedPlainDateTime.millisecond).toBe(0);
        expect(firstRoundedPlainDateTime.microsecond).toBe(0);
        expect(firstRoundedPlainDateTime.nanosecond).toBe(0);

        const secondRoundedPlainDateTime = plainDateTime.round({
            smallestUnit: "minute",
            roundingMode: "ceil",
        });
        expect(secondRoundedPlainDateTime.year).toBe(2021);
        expect(secondRoundedPlainDateTime.month).toBe(11);
        expect(secondRoundedPlainDateTime.monthCode).toBe("M11");
        expect(secondRoundedPlainDateTime.day).toBe(3);
        expect(secondRoundedPlainDateTime.hour).toBe(18);
        expect(secondRoundedPlainDateTime.minute).toBe(9);
        expect(secondRoundedPlainDateTime.second).toBe(0);
        expect(secondRoundedPlainDateTime.millisecond).toBe(0);
        expect(secondRoundedPlainDateTime.microsecond).toBe(0);
        expect(secondRoundedPlainDateTime.nanosecond).toBe(0);

        const thirdRoundedPlainDateTime = plainDateTime.round({
            smallestUnit: "minute",
            roundingMode: "ceil",
            roundingIncrement: 30,
        });
        expect(thirdRoundedPlainDateTime.year).toBe(2021);
        expect(thirdRoundedPlainDateTime.month).toBe(11);
        expect(thirdRoundedPlainDateTime.monthCode).toBe("M11");
        expect(thirdRoundedPlainDateTime.day).toBe(3);
        expect(thirdRoundedPlainDateTime.hour).toBe(18);
        expect(thirdRoundedPlainDateTime.minute).toBe(30);
        expect(thirdRoundedPlainDateTime.second).toBe(0);
        expect(thirdRoundedPlainDateTime.millisecond).toBe(0);
        expect(thirdRoundedPlainDateTime.microsecond).toBe(0);
        expect(thirdRoundedPlainDateTime.nanosecond).toBe(0);

        const fourthRoundedPlainDateTime = plainDateTime.round({
            smallestUnit: "minute",
            roundingMode: "floor",
            roundingIncrement: 30,
        });
        expect(fourthRoundedPlainDateTime.year).toBe(2021);
        expect(fourthRoundedPlainDateTime.month).toBe(11);
        expect(fourthRoundedPlainDateTime.monthCode).toBe("M11");
        expect(fourthRoundedPlainDateTime.day).toBe(3);
        expect(fourthRoundedPlainDateTime.hour).toBe(18);
        expect(fourthRoundedPlainDateTime.minute).toBe(0);
        expect(fourthRoundedPlainDateTime.second).toBe(0);
        expect(fourthRoundedPlainDateTime.millisecond).toBe(0);
        expect(fourthRoundedPlainDateTime.microsecond).toBe(0);
        expect(fourthRoundedPlainDateTime.nanosecond).toBe(0);

        const fifthRoundedPlainDateTime = plainDateTime.round({
            smallestUnit: "hour",
            roundingMode: "halfExpand",
            roundingIncrement: 4,
        });
        expect(fifthRoundedPlainDateTime.year).toBe(2021);
        expect(fifthRoundedPlainDateTime.month).toBe(11);
        expect(fifthRoundedPlainDateTime.monthCode).toBe("M11");
        expect(fifthRoundedPlainDateTime.day).toBe(3);
        expect(fifthRoundedPlainDateTime.hour).toBe(20);
        expect(fifthRoundedPlainDateTime.minute).toBe(0);
        expect(fifthRoundedPlainDateTime.second).toBe(0);
        expect(fifthRoundedPlainDateTime.millisecond).toBe(0);
        expect(fifthRoundedPlainDateTime.microsecond).toBe(0);
        expect(fifthRoundedPlainDateTime.nanosecond).toBe(0);
    });

    test("string argument is implicitly converted to options object", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);
        expect(
            plainDateTime.round("minute").equals(plainDateTime.round({ smallestUnit: "minute" }))
        ).toBeTrue();
    });

    test("range boundary conditions", () => {
        // PlainDateTime can represent a point of time ±10**8 days from the epoch.
        const min = new Temporal.PlainDateTime(-271821, 4, 19, 0, 0, 0, 0, 0, 1);
        const max = new Temporal.PlainDateTime(275760, 9, 13, 23, 59, 59, 999, 999, 999);

        ["day", "hour", "minute", "second", "millisecond", "microsecond"].forEach(smallestUnit => {
            expect(() => {
                min.round({ smallestUnit, roundingMode: "floor" });
            }).toThrow(RangeError);
            expect(() => {
                min.round({ smallestUnit, roundingMode: "ceil" });
            }).not.toThrow();

            expect(() => {
                max.round({ smallestUnit, roundingMode: "floor" });
            }).not.toThrow();
            expect(() => {
                max.round({ smallestUnit, roundingMode: "ceil" });
            }).toThrow(RangeError);
        });
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.round.call("foo", {});
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });

    test("missing options object", () => {
        expect(() => {
            const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);
            plainDateTime.round();
        }).toThrowWithMessage(TypeError, "Required options object is missing or undefined");
    });

    test("invalid rounding mode", () => {
        expect(() => {
            const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);
            plainDateTime.round({ smallestUnit: "second", roundingMode: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option roundingMode"
        );
    });

    test("invalid smallest unit", () => {
        expect(() => {
            const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);
            plainDateTime.round({ smallestUnit: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option smallestUnit"
        );
    });

    test("increment may not be NaN", () => {
        expect(() => {
            const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);
            plainDateTime.round({ smallestUnit: "second", roundingIncrement: NaN });
        }).toThrowWithMessage(RangeError, "NaN is not a valid value for option roundingIncrement");
    });

    test("increment may not be smaller than 1 or larger than maximum", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 18, 8, 10, 100, 200, 300);
        expect(() => {
            plainDateTime.round({ smallestUnit: "second", roundingIncrement: -1 });
        }).toThrowWithMessage(RangeError, "-1 is not a valid value for option roundingIncrement");
        expect(() => {
            plainDateTime.round({ smallestUnit: "second", roundingIncrement: 0 });
        }).toThrowWithMessage(RangeError, "0 is not a valid value for option roundingIncrement");
        expect(() => {
            plainDateTime.round({ smallestUnit: "second", roundingIncrement: Infinity });
        }).toThrowWithMessage(RangeError, "inf is not a valid value for option roundingIncrement");
    });
});
