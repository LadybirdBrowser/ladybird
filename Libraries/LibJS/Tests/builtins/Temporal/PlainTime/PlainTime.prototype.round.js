describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainTime.prototype.round).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);

        const firstRoundedPlainTime = plainTime.round({ smallestUnit: "minute" });
        expect(firstRoundedPlainTime.hour).toBe(18);
        expect(firstRoundedPlainTime.minute).toBe(15);
        expect(firstRoundedPlainTime.second).toBe(0);
        expect(firstRoundedPlainTime.millisecond).toBe(0);
        expect(firstRoundedPlainTime.microsecond).toBe(0);
        expect(firstRoundedPlainTime.nanosecond).toBe(0);

        const secondRoundedPlainTime = plainTime.round({
            smallestUnit: "minute",
            roundingMode: "ceil",
        });
        expect(secondRoundedPlainTime.hour).toBe(18);
        expect(secondRoundedPlainTime.minute).toBe(16);
        expect(secondRoundedPlainTime.second).toBe(0);
        expect(secondRoundedPlainTime.millisecond).toBe(0);
        expect(secondRoundedPlainTime.microsecond).toBe(0);
        expect(secondRoundedPlainTime.nanosecond).toBe(0);

        const thirdRoundedPlainTime = plainTime.round({
            smallestUnit: "minute",
            roundingMode: "ceil",
            roundingIncrement: 30,
        });
        expect(thirdRoundedPlainTime.hour).toBe(18);
        expect(thirdRoundedPlainTime.minute).toBe(30);
        expect(thirdRoundedPlainTime.second).toBe(0);
        expect(thirdRoundedPlainTime.millisecond).toBe(0);
        expect(thirdRoundedPlainTime.microsecond).toBe(0);
        expect(thirdRoundedPlainTime.nanosecond).toBe(0);

        const fourthRoundedPlainTime = plainTime.round({
            smallestUnit: "minute",
            roundingMode: "floor",
            roundingIncrement: 30,
        });
        expect(fourthRoundedPlainTime.hour).toBe(18);
        expect(fourthRoundedPlainTime.minute).toBe(0);
        expect(fourthRoundedPlainTime.second).toBe(0);
        expect(fourthRoundedPlainTime.millisecond).toBe(0);
        expect(fourthRoundedPlainTime.microsecond).toBe(0);
        expect(fourthRoundedPlainTime.nanosecond).toBe(0);

        const fifthRoundedPlainTime = plainTime.round({
            smallestUnit: "hour",
            roundingMode: "halfExpand",
            roundingIncrement: 4,
        });
        expect(fifthRoundedPlainTime.hour).toBe(20);
        expect(fifthRoundedPlainTime.minute).toBe(0);
        expect(fifthRoundedPlainTime.second).toBe(0);
        expect(fifthRoundedPlainTime.millisecond).toBe(0);
        expect(fifthRoundedPlainTime.microsecond).toBe(0);
        expect(fifthRoundedPlainTime.nanosecond).toBe(0);
    });

    test("string argument is implicitly converted to options object", () => {
        const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);
        expect(
            plainTime.round("minute").equals(plainTime.round({ smallestUnit: "minute" }))
        ).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainTime object", () => {
        expect(() => {
            Temporal.PlainTime.prototype.round.call("foo", {});
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainTime");
    });

    test("missing options object", () => {
        expect(() => {
            const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);
            plainTime.round();
        }).toThrowWithMessage(TypeError, "Required options object is missing or undefined");
    });

    test("invalid rounding mode", () => {
        expect(() => {
            const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);
            plainTime.round({ smallestUnit: "second", roundingMode: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option roundingMode"
        );
    });

    test("invalid smallest unit", () => {
        expect(() => {
            const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);
            plainTime.round({ smallestUnit: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option smallestUnit"
        );
    });

    test("increment may not be NaN", () => {
        expect(() => {
            const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);
            plainTime.round({ smallestUnit: "second", roundingIncrement: NaN });
        }).toThrowWithMessage(RangeError, "NaN is not a valid value for option roundingIncrement");
    });

    test("increment may not be smaller than 1 or larger than maximum", () => {
        const plainTime = new Temporal.PlainTime(18, 15, 9, 100, 200, 300);
        expect(() => {
            plainTime.round({ smallestUnit: "second", roundingIncrement: -1 });
        }).toThrowWithMessage(RangeError, "-1 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "second", roundingIncrement: 0 });
        }).toThrowWithMessage(RangeError, "0 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "second", roundingIncrement: Infinity });
        }).toThrowWithMessage(RangeError, "inf is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "hours", roundingIncrement: 24 });
        }).toThrowWithMessage(RangeError, "24 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "minutes", roundingIncrement: 60 });
        }).toThrowWithMessage(RangeError, "60 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "seconds", roundingIncrement: 60 });
        }).toThrowWithMessage(RangeError, "60 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "milliseconds", roundingIncrement: 1000 });
        }).toThrowWithMessage(RangeError, "1000 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "microseconds", roundingIncrement: 1000 });
        }).toThrowWithMessage(RangeError, "1000 is not a valid value for option roundingIncrement");
        expect(() => {
            plainTime.round({ smallestUnit: "nanoseconds", roundingIncrement: 1000 });
        }).toThrowWithMessage(RangeError, "1000 is not a valid value for option roundingIncrement");
    });
});
