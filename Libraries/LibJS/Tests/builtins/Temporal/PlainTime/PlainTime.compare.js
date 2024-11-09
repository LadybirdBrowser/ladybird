describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.PlainTime.compare).toHaveLength(2);
    });

    test("basic functionality", () => {
        const plainTime1 = new Temporal.PlainTime(16, 38, 40, 1, 2, 3);
        expect(Temporal.PlainTime.compare(plainTime1, plainTime1)).toBe(0);
        const plainTime2 = new Temporal.PlainTime(16, 39, 5, 0, 1, 2);
        expect(Temporal.PlainTime.compare(plainTime1, plainTime2)).toBe(-1);
        expect(Temporal.PlainTime.compare(plainTime2, plainTime1)).toBe(1);
    });
});
