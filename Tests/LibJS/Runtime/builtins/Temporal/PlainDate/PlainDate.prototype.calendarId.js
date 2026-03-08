describe("correct behavior", () => {
    test("calendarId basic functionality", () => {
        const calendar = "iso8601";
        const plainDate = new Temporal.PlainDate(2000, 5, 1, calendar);
        expect(plainDate.calendarId).toBe("iso8601");
    });

    test("non-ISO calendars", () => {
        expect(new Temporal.PlainDate(2024, 1, 1, "gregory").calendarId).toBe("gregory");
        expect(new Temporal.PlainDate(2024, 1, 1, "japanese").calendarId).toBe("japanese");
        expect(new Temporal.PlainDate(2024, 1, 1, "buddhist").calendarId).toBe("buddhist");
        expect(new Temporal.PlainDate(2024, 1, 1, "hebrew").calendarId).toBe("hebrew");
        expect(new Temporal.PlainDate(2024, 1, 1, "chinese").calendarId).toBe("chinese");
        expect(new Temporal.PlainDate(2024, 1, 1, "islamic-civil").calendarId).toBe("islamic-civil");
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainDate object", () => {
        expect(() => {
            Reflect.get(Temporal.PlainDate.prototype, "calendarId", "foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainDate");
    });
});
