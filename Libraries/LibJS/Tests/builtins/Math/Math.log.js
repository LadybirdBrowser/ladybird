test("basic functionality", () => {
    expect(Math.log).toHaveLength(1);

    expect(Math.log(-1)).toBe(NaN);
    expect(Math.log(0)).toBe(-Infinity);
    expect(Math.log(1)).toBe(0);
    expect(Math.log(10)).toBeCloseTo(2.302585092994046);
    expect(Math.log(NaN)).toBe(NaN);
    expect(Math.log(Number.POSITIVE_INFINITY)).toBe(Number.POSITIVE_INFINITY);
    expect(Math.log(-0.0)).toBe(Number.NEGATIVE_INFINITY);
});
