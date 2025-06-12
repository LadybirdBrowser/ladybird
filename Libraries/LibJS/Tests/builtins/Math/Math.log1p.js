test("basic functionality", () => {
    expect(Math.log1p).toHaveLength(1);

    expect(Math.log1p(-2)).toBeNaN();
    expect(Math.log1p(-1)).toBe(-Infinity);
    expect(Math.log1p(0)).toBe(0);
    expect(Math.log1p(1)).toBeCloseTo(0.693147);
    expect(Math.log1p(NaN)).toBe(NaN);
    expect(Math.log1p(-0.0)).toBe(-0.0);
    expect(Math.log1p(Number.POSITIVE_INFINITY)).toBe(Number.POSITIVE_INFINITY);
});
