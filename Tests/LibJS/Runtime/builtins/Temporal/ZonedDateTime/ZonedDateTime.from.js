describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.ZonedDateTime.from).toHaveLength(1);
    });

    test("ZonedDateTime instance argument", () => {
        const timeZone = "UTC";
        const calendar = "gregory";
        const zonedDateTime = new Temporal.ZonedDateTime(1627318123456789000n, timeZone, calendar);
        const createdZoneDateTime = Temporal.ZonedDateTime.from(zonedDateTime);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime).not.toBe(zonedDateTime);
        expect(createdZoneDateTime.timeZoneId).toBe(timeZone);
        expect(createdZoneDateTime.calendarId).toBe(calendar);
        expect(createdZoneDateTime.epochNanoseconds).toBe(1627318123456789000n);
    });

    test("PlainDate instance argument", () => {
        const timeZone = "UTC";
        const calendar = "gregory";
        const plainDate = new Temporal.PlainDate(2021, 11, 7, calendar);
        plainDate.timeZone = timeZone;
        const createdZoneDateTime = Temporal.ZonedDateTime.from(plainDate);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime.timeZoneId).toBe(timeZone);
        expect(createdZoneDateTime.calendarId).toBe(calendar);
        expect(createdZoneDateTime.year).toBe(2021);
        expect(createdZoneDateTime.month).toBe(11);
        expect(createdZoneDateTime.day).toBe(7);
    });

    test("PlainDateTime instance argument", () => {
        const timeZone = "UTC";
        const calendar = "gregory";
        const plainDateTime = new Temporal.PlainDateTime(2021, 11, 7, 0, 20, 5, 100, 200, 300, calendar);
        plainDateTime.timeZone = timeZone;
        const createdZoneDateTime = Temporal.ZonedDateTime.from(plainDateTime);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime.timeZoneId).toBe(timeZone);
        expect(createdZoneDateTime.calendarId).toBe(calendar);
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
        const timeZone = "UTC";
        const calendar = "gregory";
        const zonedDateTimeLike = {
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
        const createdZoneDateTime = Temporal.ZonedDateTime.from(zonedDateTimeLike);

        expect(createdZoneDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(createdZoneDateTime.timeZoneId).toBe(timeZone);
        expect(createdZoneDateTime.calendarId).toBe(calendar);
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
        const zonedDateTime = Temporal.ZonedDateTime.from("2021-11-07T00:20:05.100200300+00:00[UTC][u-ca=iso8601]");

        expect(zonedDateTime).toBeInstanceOf(Temporal.ZonedDateTime);
        expect(zonedDateTime.timeZoneId).toBe("UTC");
        expect(zonedDateTime.calendarId).toBe("iso8601");
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

    // In America/New_York, 2024-03-10T02:30 doesn't exist (spring-forward gap: 2:00 AM to 3:00 AM).
    test("DST gap disambiguation with property bag", () => {
        const gapTime = {
            year: 2024,
            month: 3,
            day: 10,
            hour: 2,
            minute: 30,
            timeZone: "America/New_York",
        };

        // "compatible": resolve to the later side of the gap (3:30 AM EDT).
        const compatible = Temporal.ZonedDateTime.from(gapTime, { disambiguation: "compatible" });
        expect(compatible.hour).toBe(3);
        expect(compatible.minute).toBe(30);
        expect(compatible.offset).toBe("-04:00");

        // "later": same as compatible for gaps.
        const later = Temporal.ZonedDateTime.from(gapTime, { disambiguation: "later" });
        expect(later.hour).toBe(3);
        expect(later.minute).toBe(30);
        expect(later.offset).toBe("-04:00");

        // "earlier": resolve to the earlier side of the gap (1:30 AM EST).
        const earlier = Temporal.ZonedDateTime.from(gapTime, { disambiguation: "earlier" });
        expect(earlier.hour).toBe(1);
        expect(earlier.minute).toBe(30);
        expect(earlier.offset).toBe("-05:00");

        // "reject": throw for non-existent times.
        expect(() => {
            Temporal.ZonedDateTime.from(gapTime, { disambiguation: "reject" });
        }).toThrowWithMessage(RangeError, "Cannot disambiguate zero possible epoch nanoseconds");
    });

    test("non-ISO calendar from property bag", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from({
            year: 2024,
            month: 6,
            day: 15,
            timeZone: "UTC",
            calendar: "gregory",
        });
        expect(zonedDateTime.calendarId).toBe("gregory");
        expect(zonedDateTime.year).toBe(2024);
        expect(zonedDateTime.month).toBe(6);
        expect(zonedDateTime.day).toBe(15);
        expect(zonedDateTime.era).toBe("ce");
        expect(zonedDateTime.eraYear).toBe(2024);
    });

    test("non-ISO calendar from string with annotation", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from("2024-06-15T12:00:00+00:00[UTC][u-ca=hebrew]");
        expect(zonedDateTime.calendarId).toBe("hebrew");
        expect(zonedDateTime.year).toBe(5784);
        expect(zonedDateTime.monthCode).toBe("M09");
    });

    test("japanese calendar with era", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from({
            year: 2024,
            month: 6,
            day: 15,
            timeZone: "Asia/Tokyo",
            calendar: "japanese",
        });
        expect(zonedDateTime.era).toBe("reiwa");
        expect(zonedDateTime.eraYear).toBe(6);
    });

    test("buddhist calendar year offset", () => {
        const zonedDateTime = Temporal.ZonedDateTime.from("2024-01-01T00:00:00+07:00[Asia/Bangkok][u-ca=buddhist]");
        expect(zonedDateTime.calendarId).toBe("buddhist");
        expect(zonedDateTime.year).toBe(2567);
    });

    test("offsets", () => {
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12].forEach(offset => {
            let timeZone = `Etc/GMT-${offset}`;
            let zonedDateTime = Temporal.ZonedDateTime.from({ year: 1970, month: 1, day: 1, timeZone: timeZone });
            expect(zonedDateTime.timeZoneId).toBe(timeZone);

            timeZone = `Etc/GMT+${offset}`;
            zonedDateTime = Temporal.ZonedDateTime.from({ year: 1970, month: 1, day: 1, timeZone: timeZone });
            expect(zonedDateTime.timeZoneId).toBe(timeZone);
        });
    });
});

describe("errors", () => {
    test("requires timeZone property", () => {
        expect(() => {
            Temporal.ZonedDateTime.from({});
        }).toThrowWithMessage(TypeError, "Required property timeZone is missing or undefined");
    });

    test("invalid zoned date time string", () => {
        expect(() => {
            Temporal.ZonedDateTime.from("foo");
        }).toThrowWithMessage(RangeError, "Invalid ISO date time");
    });

    test("invalid calendar identifier", () => {
        expect(() => {
            Temporal.ZonedDateTime.from({
                year: 2024,
                month: 1,
                day: 1,
                timeZone: "UTC",
                calendar: "invalid",
            });
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'invalid'");
    });

    test("invalid calendar annotation in string", () => {
        expect(() => {
            Temporal.ZonedDateTime.from("2024-01-01T00:00:00+00:00[UTC][u-ca=invalid]");
        }).toThrowWithMessage(RangeError, "Invalid calendar identifier 'invalid'");
    });
});
