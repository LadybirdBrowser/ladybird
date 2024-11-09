describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.Calendar.prototype.inLeapYear).toHaveLength(1);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const date = new Temporal.PlainDate(2021, 7, 23);
        expect(calendar.inLeapYear(date)).toBeFalse();
        const leapDate = new Temporal.PlainDate(2020, 7, 23);
        expect(calendar.inLeapYear(leapDate)).toBeTrue();
    });
});
