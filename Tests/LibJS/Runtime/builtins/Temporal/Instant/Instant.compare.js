describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.Instant.compare).toHaveLength(2);
    });

    test("basic functionality", () => {
        const instant1 = new Temporal.Instant(111n);
        expect(Temporal.Instant.compare(instant1, instant1)).toBe(0);
        const instant2 = new Temporal.Instant(999n);
        expect(Temporal.Instant.compare(instant1, instant2)).toBe(-1);
        expect(Temporal.Instant.compare(instant2, instant1)).toBe(1);
    });
});
