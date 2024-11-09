describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.ZonedDateTime.prototype.startOfDay).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 3, 20, 9, 45, 100, 200, 300);
        const timeZone = new Temporal.TimeZone("UTC");
        const zonedDateTime = plainDateTime.toZonedDateTime(timeZone);
        const startOfDayZonedDateTime = zonedDateTime.startOfDay();

        expect(startOfDayZonedDateTime.epochNanoseconds).toBe(1635897600000000000n);
        expect(startOfDayZonedDateTime.epochMicroseconds).toBe(1635897600000000n);
        expect(startOfDayZonedDateTime.epochMilliseconds).toBe(1635897600000);
        expect(startOfDayZonedDateTime.epochSeconds).toBe(1635897600);
        expect(startOfDayZonedDateTime.year).toBe(2021);
        expect(startOfDayZonedDateTime.month).toBe(11);
        expect(startOfDayZonedDateTime.monthCode).toBe("M11");
        expect(startOfDayZonedDateTime.day).toBe(3);
        expect(startOfDayZonedDateTime.hour).toBe(0);
        expect(startOfDayZonedDateTime.minute).toBe(0);
        expect(startOfDayZonedDateTime.second).toBe(0);
        expect(startOfDayZonedDateTime.millisecond).toBe(0);
        expect(startOfDayZonedDateTime.microsecond).toBe(0);
        expect(startOfDayZonedDateTime.nanosecond).toBe(0);
        expect(startOfDayZonedDateTime.dayOfWeek).toBe(3);
        expect(startOfDayZonedDateTime.dayOfYear).toBe(307);
        expect(startOfDayZonedDateTime.weekOfYear).toBe(44);
        expect(startOfDayZonedDateTime.hoursInDay).toBe(24);
        expect(startOfDayZonedDateTime.daysInWeek).toBe(7);
        expect(startOfDayZonedDateTime.daysInYear).toBe(365);
        expect(startOfDayZonedDateTime.monthsInYear).toBe(12);
        expect(startOfDayZonedDateTime.inLeapYear).toBeFalse();
        expect(startOfDayZonedDateTime.offset).toBe("+00:00");
        expect(startOfDayZonedDateTime.offsetNanoseconds).toBe(0);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Temporal.ZonedDateTime.prototype.startOfDay.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
