test("basic functionality", () => {
    expect(Math.log2).toHaveLength(1);

    expect(Math.log2(3)).toBeCloseTo(1.584962500721156);
    expect(Math.log2(2)).toBe(1);
    expect(Math.log2(1)).toBe(0);
    expect(Math.log2(0)).toBe(-Infinity);
    expect(Math.log2(-2)).toBe(NaN);
    expect(Math.log2(1024)).toBe(10);
    expect(Math.log2(NaN)).toBe(NaN);
    expect(Math.log2(Number.POSITIVE_INFINITY)).toBe(Number.POSITIVE_INFINITY);
    expect(Math.log2(-0.0)).toBe(Number.NEGATIVE_INFINITY);
});
