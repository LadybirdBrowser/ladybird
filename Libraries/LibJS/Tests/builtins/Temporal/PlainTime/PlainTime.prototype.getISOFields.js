describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainTime.prototype.getISOFields).toHaveLength(0);
    });

    test("basic functionality", () => {
        const plainTime = new Temporal.PlainTime(23, 53, 18, 123, 456, 789);
        const fields = plainTime.getISOFields();
        expect(fields).toEqual({
            calendar: plainTime.calendar,
            isoHour: 23,
            isoMicrosecond: 456,
            isoMillisecond: 123,
            isoMinute: 53,
            isoNanosecond: 789,
            isoSecond: 18,
        });
        // Test field order
        expect(Object.getOwnPropertyNames(fields)).toEqual([
            "calendar",
            "isoHour",
            "isoMicrosecond",
            "isoMillisecond",
            "isoMinute",
            "isoNanosecond",
            "isoSecond",
        ]);
    });
});
