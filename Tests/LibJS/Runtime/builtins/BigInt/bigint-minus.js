describe("minus behavior", () => {
    test("the basics", () => {
        expect(3n - 4n).toBe(-1n);
        expect(3n - -4n).toBe(7n);
        expect(-3n - -4n).toBe(1n);
        expect(-3n - 4n).toBe(-7n);
    });
});
