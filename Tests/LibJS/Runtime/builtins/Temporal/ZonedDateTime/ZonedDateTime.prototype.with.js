describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, "UTC");
        const values = [
            [{ year: 2021 }, 1609459200000000000n],
            [{ year: 2021, month: 7 }, 1625097600000000000n],
            [{ year: 2021, month: 7, day: 6 }, 1625529600000000000n],
            [{ year: 2021, monthCode: "M07", day: 6 }, 1625529600000000000n],
            [{ hour: 18, minute: 14, second: 47 }, 65687000000000n],
            [
                {
                    year: 2021,
                    month: 7,
                    day: 6,
                    hour: 18,
                    minute: 14,
                    second: 47,
                    millisecond: 123,
                    microsecond: 456,
                    nanosecond: 789,
                },
                1625595287123456789n,
            ],
        ];
        for (const [arg, epochNanoseconds] of values) {
            const expected = new Temporal.ZonedDateTime(epochNanoseconds, "UTC");
            expect(zonedDateTime.with(arg).equals(expected)).toBeTrue();
        }

        // Supplying the same values doesn't change the date/time, but still creates a new object
        const zonedDateTimeLike = {
            year: zonedDateTime.year,
            month: zonedDateTime.month,
            day: zonedDateTime.day,
            hour: zonedDateTime.hour,
            minute: zonedDateTime.minute,
            second: zonedDateTime.second,
            millisecond: zonedDateTime.millisecond,
            microsecond: zonedDateTime.microsecond,
            nanosecond: zonedDateTime.nanosecond,
        };
        expect(zonedDateTime.with(zonedDateTimeLike)).not.toBe(zonedDateTime);
        expect(zonedDateTime.with(zonedDateTimeLike).equals(zonedDateTime)).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("argument must be an object", () => {
        expect(() => {
            new Temporal.ZonedDateTime(0n, "UTC").with("foo");
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
        expect(() => {
            new Temporal.ZonedDateTime(0n, "UTC").with(42);
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
    });

    test("argument must have one of 'day', 'hour', 'microsecond', 'millisecond', 'minute', 'month', 'monthCode', 'nanosecond', 'second', 'year'", () => {
        expect(() => {
            new Temporal.ZonedDateTime(0n, "UTC").with({});
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
    });

    test("argument must not have 'calendar' or 'timeZone'", () => {
        expect(() => {
            new Temporal.ZonedDateTime(0n, "UTC").with({ calendar: {} });
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
        expect(() => {
            new Temporal.ZonedDateTime(0n, "UTC").with({ timeZone: {} });
        }).toThrowWithMessage(TypeError, "Object must be a partial Temporal object");
    });
});
