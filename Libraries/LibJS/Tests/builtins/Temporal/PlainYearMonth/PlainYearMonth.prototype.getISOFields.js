describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Temporal.PlainYearMonth.prototype.getISOFields).toHaveLength(0);
    });

    test("basic functionality", () => {
        const calendar = new Temporal.Calendar("iso8601");
        const plainYearMonth = new Temporal.PlainYearMonth(2021, 7, calendar, 6);
        const fields = plainYearMonth.getISOFields();
        expect(fields).toEqual({ calendar, isoDay: 6, isoMonth: 7, isoYear: 2021 });
        // Test field order
        expect(Object.getOwnPropertyNames(fields)).toEqual([
            "calendar",
            "isoDay",
            "isoMonth",
            "isoYear",
        ]);
    });
});

describe("errors", () => {
    test("this value must be a Temporal.PlainYearMonth object", () => {
        expect(() => {
            Temporal.PlainYearMonth.prototype.getISOFields.call("foo");
        }).toThrowWithMessage(TypeError, "Not an object of type Temporal.PlainYearMonth");
    });
});
