describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Now.plainDateISO).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDate = Temporal.Now.plainDateISO();
        expect(plainDate).toBeInstanceOf(Temporal.PlainDate);
        expect(plainDate.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("invalid time zone name", () => {
        expect(() => {
            Temporal.Now.plainDateISO("foo");
        }).toThrowWithMessage(RangeError, "Invalid time zone name 'foo");
    });
});
