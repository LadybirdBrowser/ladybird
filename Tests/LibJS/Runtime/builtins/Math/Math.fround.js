test("basic functionality", () => {
    expect(Math.fround).toHaveLength(1);

    expect(Math.fround(0)).toBe(0);
    expect(Math.fround(1)).toBe(1);
    expect(Math.fround(1.337)).toBeCloseTo(1.3370000123977661);
    expect(Math.fround(1.5)).toBe(1.5);
    expect(Math.fround(NaN)).toBe(NaN);
});
