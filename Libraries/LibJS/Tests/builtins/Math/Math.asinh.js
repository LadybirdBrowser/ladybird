test("basic functionality", () => {
    expect(Math.asinh).toHaveLength(1);

    expect(Math.asinh(0)).toBeCloseTo(0);
    expect(Math.asinh(1)).toBeCloseTo(0.881373);
    expect(Math.asinh(NaN)).toBe(NaN);
    expect(Math.asinh(Number.POSITIVE_INFINITY)).toBe(Number.POSITIVE_INFINITY);
    expect(Math.asinh(Number.NEGATIVE_INFINITY)).toBe(Number.NEGATIVE_INFINITY);
    expect(Math.asinh(0.0)).toBe(0.0);
    expect(Math.asinh(-0.0)).toBe(-0.0);
});
