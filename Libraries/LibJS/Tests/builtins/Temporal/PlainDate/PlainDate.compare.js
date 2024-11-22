describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.PlainDate.compare).toHaveLength(2);
    });

    test("basic functionality", () => {
        const plainDate1 = new Temporal.PlainDate(2021, 7, 26);
        expect(Temporal.PlainDate.compare(plainDate1, plainDate1)).toBe(0);
        const plainDate2 = new Temporal.PlainDate(2021, 7, 27);
        expect(Temporal.PlainDate.compare(plainDate1, plainDate2)).toBe(-1);
        expect(Temporal.PlainDate.compare(plainDate2, plainDate1)).toBe(1);
    });
});
