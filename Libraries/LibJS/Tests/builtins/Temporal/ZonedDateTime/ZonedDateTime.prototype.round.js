describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.round).toHaveLength(1);
    });

    test("basic functionality", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1111111111111n, "UTC");
        expect(zonedDateTime.round({ smallestUnit: "second" }).epochNanoseconds).toBe(
            1111000000000n
        );
        expect(
            zonedDateTime.round({ smallestUnit: "second", roundingMode: "ceil" }).epochNanoseconds
        ).toBe(1112000000000n);
        expect(
            zonedDateTime.round({
                smallestUnit: "minute",
                roundingIncrement: 30,
                roundingMode: "floor",
            }).epochNanoseconds
        ).toBe(0n);
        expect(
            zonedDateTime.round({
                smallestUnit: "minute",
                roundingIncrement: 30,
                roundingMode: "halfExpand",
            }).epochNanoseconds
        ).toBe(1800000000000n);
    });

    test("string argument is implicitly converted to options object", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1111111111111n, "UTC");
        expect(
            zonedDateTime.round("second").equals(zonedDateTime.round({ smallestUnit: "second" }))
        ).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.round.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("missing options object", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC");
        expect(() => {
            zonedDateTime.round();
        }).toThrowWithMessage(TypeError, "Required options object is missing or undefined");
    });

    test("invalid rounding mode", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC");
        expect(() => {
            zonedDateTime.round({ smallestUnit: "second", roundingMode: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option roundingMode"
        );
    });

    test("invalid smallest unit", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC");
        expect(() => {
            zonedDateTime.round({ smallestUnit: "serenityOS" });
        }).toThrowWithMessage(
            RangeError,
            "serenityOS is not a valid value for option smallestUnit"
        );
    });

    test("increment may not be NaN", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC");
        expect(() => {
            zonedDateTime.round({ smallestUnit: "second", roundingIncrement: NaN });
        }).toThrowWithMessage(RangeError, "NaN is not a valid value for option roundingIncrement");
    });

    test("increment may smaller than 1 or larger than maximum", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(1n, "UTC");
        expect(() => {
            zonedDateTime.round({ smallestUnit: "second", roundingIncrement: -1 });
        }).toThrowWithMessage(RangeError, "-1 is not a valid value for option roundingIncrement");
        expect(() => {
            zonedDateTime.round({ smallestUnit: "second", roundingIncrement: 0 });
        }).toThrowWithMessage(RangeError, "0 is not a valid value for option roundingIncrement");
        expect(() => {
            zonedDateTime.round({ smallestUnit: "second", roundingIncrement: Infinity });
        }).toThrowWithMessage(
            RangeError,
            "Infinity is not a valid value for option roundingIncrement"
        );
    });
});
