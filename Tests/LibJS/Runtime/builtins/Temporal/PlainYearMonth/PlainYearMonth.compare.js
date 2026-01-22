describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.PlainYearMonth.compare).toHaveLength(2);
    });

    test("basic functionality", () => {
        const plainYearMonth1 = new Temporal.PlainYearMonth(2021, 8);
        expect(Temporal.PlainYearMonth.compare(plainYearMonth1, plainYearMonth1)).toBe(0);
        const plainYearMonth2 = new Temporal.PlainYearMonth(2021, 9);
        expect(Temporal.PlainYearMonth.compare(plainYearMonth2, plainYearMonth2)).toBe(0);
        expect(Temporal.PlainYearMonth.compare(plainYearMonth1, plainYearMonth2)).toBe(-1);
        expect(Temporal.PlainYearMonth.compare(plainYearMonth2, plainYearMonth1)).toBe(1);
    });
});
