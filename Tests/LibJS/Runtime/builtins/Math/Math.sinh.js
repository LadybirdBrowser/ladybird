test("basic functionality", () => {
    expect(Math.sinh).toHaveLength(1);

    expect(Math.sinh(0)).toBe(0);
    expect(Math.sinh(1)).toBeCloseTo(1.1752011936438014);
    expect(Math.sinh(NaN)).toBe(NaN);
    expect(Math.sinh(Number.POSITIVE_INFINITY)).toBe(Number.POSITIVE_INFINITY);
    expect(Math.sinh(Number.NEGATIVE_INFINITY)).toBe(Number.NEGATIVE_INFINITY);
    expect(Math.sinh(-0.0)).toBe(-0.0);
});
