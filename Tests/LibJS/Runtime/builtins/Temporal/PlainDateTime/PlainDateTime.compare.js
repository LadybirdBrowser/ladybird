describe("correct behavior", () => {
    test("length is 2", () => {
        expect(Temporal.PlainDateTime.compare).toHaveLength(2);
    });

    test("basic functionality", () => {
        const plainDateTime1 = new Temporal.PlainDateTime(2021, 8, 27, 16, 38, 40, 1, 2, 3);
        expect(Temporal.PlainDateTime.compare(plainDateTime1, plainDateTime1)).toBe(0);
        const plainDateTime2 = new Temporal.PlainDateTime(2021, 8, 27, 16, 39, 5, 0, 1, 2);
        expect(Temporal.PlainDateTime.compare(plainDateTime1, plainDateTime2)).toBe(-1);
        expect(Temporal.PlainDateTime.compare(plainDateTime2, plainDateTime1)).toBe(1);
    });
});
