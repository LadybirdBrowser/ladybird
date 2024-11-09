describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.month).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(calendar.month(date)).toBe(7);
    });
});

describe("errors", () => {
    test("argument must not be a Temporal.PlainMonthDay object", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainMonthDay = new Temporal.PlainMonthDay(7, 6);
        expect(() => {
            calendar.month(plainMonthDay);
        }).toThrowWithMessage(
            TypeError,
            "Accessing month of PlainMonthDay is ambiguous, use monthCode instead"
        );
    });
});
