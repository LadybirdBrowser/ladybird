describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.with).toHaveLength(1);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(1970, 1, 1);
        const values = [
            [{ year: 2021 }, new Temporal.PlainDateTime(2021, 1, 1)],
            [{ year: 2021, month: 7 }, new Temporal.PlainDateTime(2021, 7, 1)],
            [{ year: 2021, month: 7, day: 6 }, new Temporal.PlainDateTime(2021, 7, 6)],
            [{ year: 2021, monthCode: "M07", day: 6 }, new Temporal.PlainDateTime(2021, 7, 6)],
            [
                { hour: 18, minute: 14, second: 47 },
                new Temporal.PlainDateTime(1970, 1, 1, 18, 14, 47),
            ],
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
                new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47, 123, 456, 789),
            ],
        ];
        for (const [arg, expected] of values) {
            expect(plainDateTime.with(arg).equals(expected)).toBeTrue();
        }

        // Supplying the same values doesn't change the date/time, but still creates a new object
        const plainDateTimeLike = {
            year: plainDateTime.year,
            month: plainDateTime.month,
            day: plainDateTime.day,
            hour: plainDateTime.hour,
            minute: plainDateTime.minute,
            second: plainDateTime.second,
            millisecond: plainDateTime.millisecond,
            microsecond: plainDateTime.microsecond,
            nanosecond: plainDateTime.nanosecond,
        };
        expect(plainDateTime.with(plainDateTimeLike)).not.toBe(plainDateTime);
        expect(plainDateTime.with(plainDateTimeLike).equals(plainDateTime)).toBeTrue();
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDateTime object", () => {
        expect(() => {
            Temporal.PlainDateTime.prototype.with.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDateTime");
    });

    test("argument must be an object", () => {
        expect(() => {
            new Temporal.PlainDateTime(1970, 1, 1).with("foo");
        }).toThrowWithMessage(TypeError, "foo is not an object");
        expect(() => {
            new Temporal.PlainDateTime(1970, 1, 1).with(42);
        }).toThrowWithMessage(TypeError, "42 is not an object");
    });

    test("argument must have one of 'day', 'hour', 'microsecond', 'millisecond', 'minute', 'month', 'monthCode', 'nanosecond', 'second', 'year'", () => {
        expect(() => {
            new Temporal.PlainDateTime(1970, 1, 1).with({});
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: day, hour, microsecond, millisecond, minute, month, monthCode, nanosecond, second, year"
        );
    });

    test("argument must not have 'calendar' or 'timeZone'", () => {
        expect(() => {
            new Temporal.PlainDateTime(1970, 1, 1).with({ calendar: {} });
        }).toThrowWithMessage(TypeError, "Object must not have a defined calendar property");
        expect(() => {
            new Temporal.PlainDateTime(1970, 1, 1).with({ timeZone: {} });
        }).toThrowWithMessage(TypeError, "Object must not have a defined timeZone property");
    });
});
