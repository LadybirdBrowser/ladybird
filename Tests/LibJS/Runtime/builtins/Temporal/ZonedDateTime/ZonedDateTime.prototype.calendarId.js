describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const timeZone = "UTC";
        const calendar = "gregory";
        const zonedDateTime = new Temporal.ZonedDateTime(0n, timeZone, calendar);
        expect(zonedDateTime.calendarId).toBe("gregory");
    });

    test("non-ISO calendars", () => {
        expect(new Temporal.ZonedDateTime(0n, "UTC", "japanese").calendarId).toBe("japanese");
        expect(new Temporal.ZonedDateTime(0n, "UTC", "buddhist").calendarId).toBe("buddhist");
        expect(new Temporal.ZonedDateTime(0n, "UTC", "hebrew").calendarId).toBe("hebrew");
        expect(new Temporal.ZonedDateTime(0n, "UTC", "chinese").calendarId).toBe("chinese");
        expect(new Temporal.ZonedDateTime(0n, "UTC", "islamic-civil").calendarId).toBe("islamic-civil");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.ZonedDateTime object", () => {
        expect(() => {
            Reflect.get(Temporal.ZonedDateTime.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.ZonedDateTime");
    });
});
