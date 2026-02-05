describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Now.plainDateTimeISO).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainDateTime = Temporal.Now.plainDateTimeISO();
        expect(plainDateTime).toBeInstanceOf(Temporal.PlainDateTime);
        expect(plainDateTime.calendarId).toBe("iso8601");
    });
});

describe("errors", () => {
    test("invalid time zone name", () => {
        expect(() => {
            Temporal.Now.plainDateTimeISO("foo");
        }).toThrowWithMessage(RangeError, "Invalid time zone name 'foo");
    });
});
