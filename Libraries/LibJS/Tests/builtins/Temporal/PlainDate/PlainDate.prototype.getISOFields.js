describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainDate.prototype.getISOFields).toHaveLength(0);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainDate = new Temporal.PlainDate(2021, 7, 29, calendar);
        const fields = plainDate.getISOFields();
        expect(fields).toEqual({ calendar, isoDay: 29, isoMonth: 7, isoYear: 2021 });
        // Test field order
        expect(Object.getOwnPropertyNames(fields)).toEqual([
            "calendar",
            "isoDay",
            "isoMonth",
            "isoYear",
        ]);
    });
});
