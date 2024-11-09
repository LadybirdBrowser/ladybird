describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.from).toHaveLength(1);
    });

    test("PlainDate instance argument", () => {
        const plainDate = new Temporal.PlainDate(2021, 7, 6);
        const plainDateTime = Temporal.PlainDateTime.from(plainDate);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(6);
        expect(plainDateTime.hour).toBe(0);
        expect(plainDateTime.minute).toBe(0);
        expect(plainDateTime.second).toBe(0);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
    });

    test("PlainDateTime instance argument", () => {
        const plainDateTime_ = new Temporal.PlainDateTime(2021, 7, 6, 18, 14, 47);
        const plainDateTime = Temporal.PlainDateTime.from(plainDateTime_);
        expect(plainDateTime).not.toBe(plainDateTime_);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(6);
        expect(plainDateTime.hour).toBe(18);
        expect(plainDateTime.minute).toBe(14);
        expect(plainDateTime.second).toBe(47);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
    });

    test("ZonedDateTime instance argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = new Temporal.ZonedDateTime(1625614921000000000n, timeZone);
        const plainDateTime = Temporal.PlainDateTime.from(zonedDateTime);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(6);
        expect(plainDateTime.hour).toBe(23);
        expect(plainDateTime.minute).toBe(42);
        expect(plainDateTime.second).toBe(1);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
    });

    test("fields object argument", () => {
        const object = {
            year: 2021,
            month: 7,
            day: 6,
            hour: 23,
            minute: 42,
            second: 1,
            millisecond: 0,
            microsecond: 0,
            nanosecond: 0,
        };
        const plainDateTime = Temporal.PlainDateTime.from(object);
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(6);
        expect(plainDateTime.hour).toBe(23);
        expect(plainDateTime.minute).toBe(42);
        expect(plainDateTime.second).toBe(1);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
    });

    test("with 'constrain' overflow option", () => {
        const object = {
            year: 0,
            month: 1,
            day: 1,
            hour: 24,
            minute: 60,
            second: 60,
            millisecond: 1000,
            microsecond: 1000,
            nanosecond: 1000,
        };
        const plainDateTime = Temporal.PlainDateTime.from(object, { overflow: "constrain" });
        expect(plainDateTime.year).toBe(0);
        expect(plainDateTime.month).toBe(1);
        expect(plainDateTime.day).toBe(1);
        expect(plainDateTime.hour).toBe(23);
        expect(plainDateTime.minute).toBe(59);
        expect(plainDateTime.second).toBe(59);
        expect(plainDateTime.millisecond).toBe(999);
        expect(plainDateTime.microsecond).toBe(999);
        expect(plainDateTime.nanosecond).toBe(999);
    });

    test("PlainDateTime string argument", () => {
        const plainDateTime = Temporal.PlainDateTime.from("2021-07-06T23:42:01");
        expect(plainDateTime.year).toBe(2021);
        expect(plainDateTime.month).toBe(7);
        expect(plainDateTime.day).toBe(6);
        expect(plainDateTime.hour).toBe(23);
        expect(plainDateTime.minute).toBe(42);
        expect(plainDateTime.second).toBe(1);
        expect(plainDateTime.millisecond).toBe(0);
        expect(plainDateTime.microsecond).toBe(0);
        expect(plainDateTime.nanosecond).toBe(0);
    });
});

describe("errors", () => {
    test("missing fields", () => {
        expect(() => {
            Temporal.PlainDateTime.from({});
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
        expect(() => {
            Temporal.PlainDateTime.from({ year: 0, day: 1 });
        }).toThrowWithMessage(TypeError, "Required property month is missing or undefined");
        expect(() => {
            Temporal.PlainDateTime.from({ month: 1, day: 1 });
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
    });

    test("with 'reject' overflow option", () => {
        const values = [
            [{ year: 1234567, month: 1, day: 1 }, "Invalid plain date"],
            [{ year: 0, month: 13, day: 1 }, "Invalid plain date"],
            [{ year: 0, month: 1, day: 32 }, "Invalid plain date"],
            [{ year: 0, month: 1, day: 1, hour: 24 }, "Invalid plain time"],
            [{ year: 0, month: 1, day: 1, hour: 0, minute: 60 }, "Invalid plain time"],
            [{ year: 0, month: 1, day: 1, hour: 0, minute: 0, second: 60 }, "Invalid plain time"],
            [
                { year: 0, month: 1, day: 1, hour: 0, minute: 0, second: 0, millisecond: 1000 },
                "Invalid plain time",
            ],
            [
                {
                    year: 0,
                    month: 1,
                    day: 1,
                    hour: 0,
                    minute: 0,
                    second: 0,
                    millisecond: 0,
                    microsecond: 1000,
                },
                "Invalid plain time",
            ],
            [
                {
                    year: 0,
                    month: 1,
                    day: 1,
                    hour: 0,
                    minute: 0,
                    second: 0,
                    millisecond: 0,
                    microsecond: 0,
                    nanosecond: 1000,
                },
                "Invalid plain time",
            ],
        ];
        for (const [object, error] of values) {
            expect(() => {
                Temporal.PlainDateTime.from(object, { overflow: "reject" });
            }).toThrowWithMessage(RangeError, error);
        }
    });
});

describe("errors", () => {
    test("custom time zone doesn't have a getOffsetNanosecondsFor function", () => {
        const zonedDateTime = new Temporal.ZonedDateTime(0n, {});
        expect(() => {
            Temporal.PlainDateTime.from(zonedDateTime);
        }).toThrowWithMessage(TypeError, "getOffsetNanosecondsFor is undefined");
    });

    test("string must not contain a UTC designator", () => {
        expect(() => {
            Temporal.PlainDateTime.from("2021-07-06T23:42:01Z");
        }).toThrowWithMessage(
            RangeError,
            "Invalid date time string '2021-07-06T23:42:01Z': must not contain a UTC designator"
        );
    });

    test("extended year must not be negative zero", () => {
        expect(() => {
            Temporal.PlainDateTime.from("-000000-01-01");
        }).toThrowWithMessage(RangeError, "Invalid date time string '-000000-01-01'");
        expect(() => {
            Temporal.PlainDateTime.from("−000000-01-01"); // U+2212
        }).toThrowWithMessage(RangeError, "Invalid date time string '−000000-01-01'");
    });
});
