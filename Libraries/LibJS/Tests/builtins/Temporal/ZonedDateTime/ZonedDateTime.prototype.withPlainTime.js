describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.withPlainTime).toHaveLength(0);
    });

    function checkCommonExpectedResults(withPlainTimeZonedDateTime) {
        expect(withPlainTimeZonedDateTime.epochNanoseconds).toBe(1636064604200300400n);
        expect(withPlainTimeZonedDateTime.epochMicroseconds).toBe(1636064604200300n);
        expect(withPlainTimeZonedDateTime.epochMilliseconds).toBe(1636064604200);
        expect(withPlainTimeZonedDateTime.epochSeconds).toBe(1636064604);
        expect(withPlainTimeZonedDateTime.year).toBe(2021);
        expect(withPlainTimeZonedDateTime.month).toBe(11);
        expect(withPlainTimeZonedDateTime.monthCode).toBe("M11");
        expect(withPlainTimeZonedDateTime.day).toBe(4);
        expect(withPlainTimeZonedDateTime.hour).toBe(22);
        expect(withPlainTimeZonedDateTime.minute).toBe(23);
        expect(withPlainTimeZonedDateTime.second).toBe(24);
        expect(withPlainTimeZonedDateTime.millisecond).toBe(200);
        expect(withPlainTimeZonedDateTime.microsecond).toBe(300);
        expect(withPlainTimeZonedDateTime.nanosecond).toBe(400);
        expect(withPlainTimeZonedDateTime.dayOfWeek).toBe(4);
        expect(withPlainTimeZonedDateTime.dayOfYear).toBe(308);
        expect(withPlainTimeZonedDateTime.weekOfYear).toBe(44);
        expect(withPlainTimeZonedDateTime.hoursInDay).toBe(24);
        expect(withPlainTimeZonedDateTime.daysInWeek).toBe(7);
        expect(withPlainTimeZonedDateTime.daysInYear).toBe(365);
        expect(withPlainTimeZonedDateTime.monthsInYear).toBe(12);
        expect(withPlainTimeZonedDateTime.inLeapYear).toBeFalse();
        expect(withPlainTimeZonedDateTime.offset).toBe("+00:00");
        expect(withPlainTimeZonedDateTime.offsetNanoseconds).toBe(0);
    }

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const plainTime = new Temporal.PlainTime(22, 23, 24, 200, 300, 400);
        const withPlainTimeZonedDateTime = zonedDateTime.withPlainTime(plainTime);

        checkCommonExpectedResults(withPlainTimeZonedDateTime);
    });

    test("plain time-like object", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const plainTimeLike = {
            hour: 22,
            minute: 23,
            second: 24,
            millisecond: 200,
            microsecond: 300,
            nanosecond: 400,
        };
        const withPlainTimeZonedDateTime = zonedDateTime.withPlainTime(plainTimeLike);

        checkCommonExpectedResults(withPlainTimeZonedDateTime);
    });

    test("passing no parameters is the equivalent of using startOfDay", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const startOfDayZonedDateTime = zonedDateTime.startOfDay();
        const withPlainTimeZonedDateTime = zonedDateTime.withPlainTime();

        expect(startOfDayZonedDateTime.epochNanoseconds).toBe(
            withPlainTimeZonedDateTime.epochNanoseconds
        );
        expect(startOfDayZonedDateTime.epochMicroseconds).toBe(
            withPlainTimeZonedDateTime.epochMicroseconds
        );
        expect(startOfDayZonedDateTime.epochMilliseconds).toBe(
            withPlainTimeZonedDateTime.epochMilliseconds
        );
        expect(startOfDayZonedDateTime.epochSeconds).toBe(withPlainTimeZonedDateTime.epochSeconds);
        expect(startOfDayZonedDateTime.year).toBe(withPlainTimeZonedDateTime.year);
        expect(startOfDayZonedDateTime.month).toBe(withPlainTimeZonedDateTime.month);
        expect(startOfDayZonedDateTime.monthCode).toBe(withPlainTimeZonedDateTime.monthCode);
        expect(startOfDayZonedDateTime.day).toBe(withPlainTimeZonedDateTime.day);
        expect(startOfDayZonedDateTime.hour).toBe(withPlainTimeZonedDateTime.hour);
        expect(startOfDayZonedDateTime.minute).toBe(withPlainTimeZonedDateTime.minute);
        expect(startOfDayZonedDateTime.second).toBe(withPlainTimeZonedDateTime.second);
        expect(startOfDayZonedDateTime.millisecond).toBe(withPlainTimeZonedDateTime.millisecond);
        expect(startOfDayZonedDateTime.microsecond).toBe(withPlainTimeZonedDateTime.microsecond);
        expect(startOfDayZonedDateTime.nanosecond).toBe(withPlainTimeZonedDateTime.nanosecond);
        expect(startOfDayZonedDateTime.dayOfWeek).toBe(withPlainTimeZonedDateTime.dayOfWeek);
        expect(startOfDayZonedDateTime.dayOfYear).toBe(withPlainTimeZonedDateTime.dayOfYear);
        expect(startOfDayZonedDateTime.weekOfYear).toBe(withPlainTimeZonedDateTime.weekOfYear);
        expect(startOfDayZonedDateTime.hoursInDay).toBe(withPlainTimeZonedDateTime.hoursInDay);
        expect(startOfDayZonedDateTime.daysInWeek).toBe(withPlainTimeZonedDateTime.daysInWeek);
        expect(startOfDayZonedDateTime.daysInYear).toBe(withPlainTimeZonedDateTime.daysInYear);
        expect(startOfDayZonedDateTime.monthsInYear).toBe(withPlainTimeZonedDateTime.monthsInYear);
        expect(startOfDayZonedDateTime.inLeapYear).toBe(withPlainTimeZonedDateTime.inLeapYear);
        expect(startOfDayZonedDateTime.offset).toBe(withPlainTimeZonedDateTime.offset);
        expect(startOfDayZonedDateTime.offsetNanoseconds).toBe(
            withPlainTimeZonedDateTime.offsetNanoseconds
        );
    });

    test("from plain time string", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 4, 21, 16, 56, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const withPlainTimeZonedDateTime = zonedDateTime.withPlainTime("22:23:24.200300400");

        checkCommonExpectedResults(withPlainTimeZonedDateTime);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.withPlainTime.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });

    test("invalid plain time-like object", () => {
        expect(() => {
            new Temporal.ZonedDateTime(1n, {}).withPlainTime({});
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: hour, microsecond, millisecond, minute, nanosecond, second"
        );

        expect(() => {
            new Temporal.ZonedDateTime(1n, {}).withPlainTime({ foo: 1, bar: 2 });
        }).toThrowWithMessage(
            TypeError,
            "Object must have at least one of the following properties: hour, microsecond, millisecond, minute, nanosecond, second"
        );
    });
});
