describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.from).toHaveLength(1);
    });

    test("ZonedDateTime instance argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = new Temporal.Calendar("iso8601");
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, timeZone, calendar);
        const createdZoneDateTime = Temporal.ZonedDateTime.from(zonedDateTime);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime).not.toBe(zonedDateTime);
        expect(createdZoneDateTime.timeZone).toBe(timeZone);
        expect(createdZoneDateTime.calendar).toBe(calendar);
        expect(createdZoneDateTime.epochNanoseconds).toBe(1627318123456789000n);
    });

    test("PlainDate instance argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = new Temporal.Calendar("iso8601");
        const plainDate = new Temporal.PlainDate(2021, 11, 7, calendar);
        plainDate.timeZone = timeZone;
        const createdZoneDateTime = Temporal.ZonedDateTime.from(plainDate);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime.timeZone).toBe(timeZone);
        expect(createdZoneDateTime.calendar).toBe(calendar);
        expect(createdZoneDateTime.year).toBe(2021);
        expect(createdZoneDateTime.month).toBe(11);
        expect(createdZoneDateTime.day).toBe(7);
    });

    test("PlainDateTime instance argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = new Temporal.Calendar("iso8601");
        const plainDateTime = new Temporal.PlainDateTime(
            2021,
            11,
            7,
            0,
            20,
            5,
            100,
            200,
            300,
            calendar
        );
        plainDateTime.timeZone = timeZone;
        const createdZoneDateTime = Temporal.ZonedDateTime.from(plainDateTime);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime.timeZone).toBe(timeZone);
        expect(createdZoneDateTime.calendar).toBe(calendar);
        expect(createdZoneDateTime.year).toBe(2021);
        expect(createdZoneDateTime.month).toBe(11);
        expect(createdZoneDateTime.day).toBe(7);
        expect(createdZoneDateTime.hour).toBe(0);
        expect(createdZoneDateTime.minute).toBe(20);
        expect(createdZoneDateTime.second).toBe(5);
        expect(createdZoneDateTime.millisecond).toBe(100);
        expect(createdZoneDateTime.microsecond).toBe(200);
        expect(createdZoneDateTime.nanosecond).toBe(300);
    });

    test("ZonedDateTime-like argument", () => {
        const timeZone = new Temporal.TimeZone("UTC");
        const calendar = new Temporal.Calendar("iso8601");
        const zdtLike = {
            timeZone,
            calendar,
            year: 2021,
            month: 11,
            day: 7,
            hour: 0,
            minute: 20,
            second: 5,
            millisecond: 100,
            microsecond: 200,
            nanosecond: 300,
        };
        const createdZoneDateTime = Temporal.ZonedDateTime.from(zdtLike);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime.timeZone).toBe(timeZone);
        expect(createdZoneDateTime.calendar).toBe(calendar);
        expect(createdZoneDateTime.year).toBe(2021);
        expect(createdZoneDateTime.month).toBe(11);
        expect(createdZoneDateTime.day).toBe(7);
        expect(createdZoneDateTime.hour).toBe(0);
        expect(createdZoneDateTime.minute).toBe(20);
        expect(createdZoneDateTime.second).toBe(5);
        expect(createdZoneDateTime.millisecond).toBe(100);
        expect(createdZoneDateTime.microsecond).toBe(200);
        expect(createdZoneDateTime.nanosecond).toBe(300);
    });

    test("from string", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from(
            "2021-11-07T00:20:05.100200300+00:00[UTC][u-ca=iso8601]"
        );

        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(zonedDateTime.timeZone).toBeInstanceOf(Temporal.TimeZone);
        expect(zonedDateTime.timeZone.id).toBe("UTC");
        expect(zonedDateTime.calendar).toBeInstanceOf(Temporal.Calendar);
        expect(zonedDateTime.calendar.id).toBe("iso8601");
        expect(zonedDateTime.year).toBe(2021);
        expect(zonedDateTime.month).toBe(11);
        expect(zonedDateTime.day).toBe(7);
        expect(zonedDateTime.hour).toBe(0);
        expect(zonedDateTime.minute).toBe(20);
        expect(zonedDateTime.second).toBe(5);
        expect(zonedDateTime.millisecond).toBe(100);
        expect(zonedDateTime.microsecond).toBe(200);
        expect(zonedDateTime.nanosecond).toBe(300);
        expect(zonedDateTime.offset).toBe("+00:00");
        expect(zonedDateTime.offsetNanoseconds).toBe(0);
    });
});

describe("errors", () => {
    test("requires timeZone property", () => {
        expect(() => {
            Temporal.ZonedDateTime.from({});
        }).toThrowWithMessage(TypeError, "Required property timeZone is missing or undefined");
    });

    test("requires year property", () => {
        expect(() => {
            Temporal.ZonedDateTime.from({ timeZone: new Temporal.TimeZone("UTC") });
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
    });

    test("requires month property", () => {
        expect(() => {
            Temporal.ZonedDateTime.from({
                timeZone: new Temporal.TimeZone("UTC"),
                day: 1,
                year: 2021,
            });
        }).toThrowWithMessage(TypeError, "Required property month is missing or undefined");
    });

    test("requires day property", () => {
        expect(() => {
            Temporal.ZonedDateTime.from({
                timeZone: new Temporal.TimeZone("UTC"),
                year: 2021,
                month: 11,
            });
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");
    });

    test("invalid zoned date time string", () => {
        expect(() => {
            Temporal.ZonedDateTime.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid zoned date time string 'foo'");
    });
});
