describe("correct behavior", () => {
    test("length is 0", () => {
        expect(Temporal.Now.plainTimeISO).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = Temporal.Now.plainTimeISO();
        expect(plainTime).toBeInstanceOf(Temporal.PlainTime);
        expect(plainTime.calendarId).toBeUndefined();
    });
});

describe("errors", () => {
    test("invalid time zone name", () => {
        expect(() => {
            Temporal.Now.plainTimeISO("foo");
        }).toThrowWithMessage(RangeError, "Invalid time zone name 'foo");
    });
});
