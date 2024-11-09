describe("correct behavior", () => {
    test("length is 1", () => {
        expect(Temporal.PlainDateTime.prototype.withPlainDate).toHaveLength(1);
    });

    test("basic functionality", () => {
        const firstPlainDateTime = new Temporal.PlainDateTime(1, 2, 3, 4, 5, 6);
        const plainDate = new Temporal.PlainDate(7, 8, 9);
        const secondPlainDateTime = firstPlainDateTime.withPlainDate(plainDate);
        expect(secondPlainDateTime.year).toBe(7);
        expect(secondPlainDateTime.month).toBe(8);
        expect(secondPlainDateTime.day).toBe(9);
        expect(secondPlainDateTime.hour).toBe(4);
        expect(secondPlainDateTime.minute).toBe(5);
        expect(secondPlainDateTime.second).toBe(6);
    });
});
