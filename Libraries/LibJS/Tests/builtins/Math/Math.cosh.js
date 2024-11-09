describe("basic functionality", () => {
    test("length", () => {
        expect(Math.cosh).toHaveLength(1);
    });

    test("simple values", () => {
        expect(Math.cosh(1)).toBeCloseTo(1.5430806348152437);
        expect(Math.cosh(-1)).toBeCloseTo(1.5430806348152437);
    });

    test("special values", () => {
        expect(Math.cosh(0)).toBe(1);
        expect(Math.cosh(-0.0)).toBe(1);

        expect(Math.cosh(NaN)).toBeNaN();
        expect(Math.cosh(Infinity)).toBe(Infinity);
        expect(Math.cosh(-Infinity)).toBe(Infinity);
    });
});
