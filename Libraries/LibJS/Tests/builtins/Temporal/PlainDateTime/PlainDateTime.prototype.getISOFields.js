describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDateTime.prototype.getISOFields).toHaveLength(0);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainDateTime = new Temporal.PlainDateTime(
            2021,
            7,
            23,
            0,
            42,
            18,
            123,
            456,
            789,
            calendar
        );
        const fields = plainDateTime.getISOFields();
        expect(fields).toEqual({
            calendar: calendar,
            isoDay: 23,
            isoHour: 0,
            isoMicrosecond: 456,
            isoMillisecond: 123,
            isoMinute: 42,
            isoMonth: 7,
            isoNanosecond: 789,
            isoSecond: 18,
            isoYear: 2021,
        });
        // Test field order
        expect(Object.getOwnPropertyNames(fields)).toEqual([
            "calendar",
            "isoDay",
            "isoHour",
            "isoMicrosecond",
            "isoMillisecond",
            "isoMinute",
            "isoMonth",
            "isoNanosecond",
            "isoSecond",
            "isoYear",
        ]);
    });
});
