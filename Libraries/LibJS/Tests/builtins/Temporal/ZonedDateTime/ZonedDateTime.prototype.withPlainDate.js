describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.prototype.withPlainDate).toHaveLength(1);
    });

    function checkExpectedResults(withPlainDateZonedDateTime) {
        expect(withPlainDateZonedDateTime.epochNanoseconds).toBe(1640467016100200300n);
        expect(withPlainDateZonedDateTime.epochMicroseconds).toBe(1640467016100200n);
        expect(withPlainDateZonedDateTime.epochMilliseconds).toBe(1640467016100);
        expect(withPlainDateZonedDateTime.epochSeconds).toBe(1640467016);
        expect(withPlainDateZonedDateTime.year).toBe(2021);
        expect(withPlainDateZonedDateTime.month).toBe(12);
        expect(withPlainDateZonedDateTime.monthCode).toBe("M12");
        expect(withPlainDateZonedDateTime.day).toBe(25);
        expect(withPlainDateZonedDateTime.hour).toBe(21);
        expect(withPlainDateZonedDateTime.minute).toBe(16);
        expect(withPlainDateZonedDateTime.second).toBe(56);
        expect(withPlainDateZonedDateTime.millisecond).toBe(100);
        expect(withPlainDateZonedDateTime.microsecond).toBe(200);
        expect(withPlainDateZonedDateTime.nanosecond).toBe(300);
        expect(withPlainDateZonedDateTime.dayOfWeek).toBe(6);
        expect(withPlainDateZonedDateTime.dayOfYear).toBe(359);
        expect(withPlainDateZonedDateTime.weekOfYear).toBe(51);
        expect(withPlainDateZonedDateTime.hoursInDay).toBe(24);
        expect(withPlainDateZonedDateTime.daysInWeek).toBe(7);
        expect(withPlainDateZonedDateTime.daysInYear).toBe(365);
        expect(withPlainDateZonedDateTime.monthsInYear).toBe(12);
        expect(withPlainDateZonedDateTime.inLeapYear).toBeFalse();
        expect(withPlainDateZonedDateTime.offset).toBe("+00:00");
        expect(withPlainDateZonedDateTime.offsetNanoseconds).toBe(0);
    }

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const plainDate = new Temporal.PlainDate(2021, 12, 25);
        const withPlainDateZonedDateTime = zonedDateTime.withPlainDate(plainDate);

        checkExpectedResults(withPlainDateZonedDateTime);
    });

    test("plain time-like object", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const plainDateLike = { year: 2021, month: 12, day: 25 };
        const withPlainDateZonedDateTime = zonedDateTime.withPlainDate(plainDateLike);

        checkExpectedResults(withPlainDateZonedDateTime);
    });

    test("from plain date string", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const withPlainDateZonedDateTime = zonedDateTime.withPlainDate("2021-12-25");

        checkExpectedResults(withPlainDateZonedDateTime);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.withPlainDate.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("missing properties", () => {
        expect(() => {
            new Temporal.ZonedDateTime(1n, {}).withPlainDate({});
        }).toThrowWithMessage(TypeError, "Required property day is missing or undefined");

        expect(() => {
            new Temporal.ZonedDateTime(1n, {}).withPlainDate({ day: 1, year: 1 });
        }).toThrowWithMessage(TypeError, "Required property month is missing or undefined");

        expect(() => {
            new Temporal.ZonedDateTime(1n, {}).withPlainDate({ day: 1, month: 1 });
        }).toThrowWithMessage(TypeError, "Required property year is missing or undefined");
    });
});
