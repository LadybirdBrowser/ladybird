const expectDurationOneToTen = duration => {
    expect(duration.years).toBe(1);
    expect(duration.months).toBe(2);
    expect(duration.weeks).toBe(3);
    expect(duration.days).toBe(4);
    expect(duration.hours).toBe(5);
    expect(duration.minutes).toBe(6);
    expect(duration.seconds).toBe(7);
    expect(duration.milliseconds).toBe(8);
    expect(duration.microseconds).toBe(9);
    expect(duration.nanoseconds).toBe(10);
};

describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Duration.from).toHaveLength(1);
    });

    test("Duration instance argument", () => {
        const duration = Temporal.Duration.from(
            new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
        );
        expectDurationOneToTen(duration);
    });

    test("Duration-like object argument", () => {
        const duration = Temporal.Duration.from({
            years: 1,
            months: 2,
            weeks: 3,
            days: 4,
            hours: 5,
            minutes: 6,
            seconds: 7,
            milliseconds: 8,
            microseconds: 9,
            nanoseconds: 10,
        });
        expectDurationOneToTen(duration);
    });

    test("Duration string argument", () => {
        const duration = Temporal.Duration.from("P1Y2M3W4DT5H6M7.008009010S");
        expectDurationOneToTen(duration);
    });
});

describe("errors", () => {
    test("Invalid duration-like object", () => {
        expect(() => {
            Temporal.Duration.from({});
        }).toThrowWithMessage(TypeError, "Invalid duration-like object");
    });

    test("Invalid duration property value", () => {
        expect(() => {
            Temporal.Duration.from({ years: 1.23 });
        }).toThrowWithMessage(
            RangeError,
            "Invalid value for duration property 'years': must be an integer, got 1.2" // ...29999999999999 - let's not include that in the test :^)
        );

        expect(() => {
            Temporal.Duration.from({ years: "foo" });
        }).toThrowWithMessage(
            RangeError,
            "Invalid value for duration property 'years': must be an integer, got foo"
        );

        expect(() => {
            Temporal.Duration.from({ years: NaN });
        }).toThrowWithMessage(
            RangeError,
            "Invalid value for duration property 'years': must be an integer, got NaN"
        );
    });

    test("invalid duration string", () => {
        expect(() => {
            Temporal.Duration.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid duration string 'foo'");
    });

    test("invalid duration string: fractional hours proceeded by minutes or seconds", () => {
        const values = [
            "PT1.23H1M",
            "PT1.23H1.23M",
            "PT1.23H1S",
            "PT1.23H1.23S",
            "PT1.23H1M1S",
            "PT1.23H1M1.23S",
            "PT1.23H1.23M1S",
            "PT1.23H1.23M1.23S",
        ];
        for (const value of values) {
            expect(() => {
                Temporal.Duration.from(value);
            }).toThrowWithMessage(RangeError, `Invalid duration string '${value}'`);
        }
    });

    test("invalid duration string: fractional minutes proceeded by seconds", () => {
        const values = ["PT1.23M1S", "PT1.23M1.23S"];
        for (const value of values) {
            expect(() => {
                Temporal.Duration.from(value);
            }).toThrowWithMessage(RangeError, `Invalid duration string '${value}'`);
        }
    });

    test("invalid duration string: exceed duration limits", () => {
        const values = [
            "P4294967296Y", // abs(years) >= 2**32
            "P4294967296M", // abs(months) >= 2**32
            "P4294967296W", // abs(weeks) >= 2**32
            "P104249991375D", // days >= 2*53 seconds
            "PT2501999792984H", // hours >= 2*53 seconds
            "PT150119987579017M", // minutes >= 2*53 seconds
            "PT9007199254740992S", // seconds >= 2*53 seconds
        ];

        for (const value of values) {
            expect(() => {
                Temporal.Duration.from(value);
            }).toThrowWithMessage(RangeError, `Invalid duration`);

            expect(() => {
                Temporal.Duration.from("-" + value);
            }).toThrowWithMessage(RangeError, `Invalid duration`);
        }
    });
});
