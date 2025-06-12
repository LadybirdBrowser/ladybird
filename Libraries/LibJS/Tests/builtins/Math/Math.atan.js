test("basic functionality", () => {
    expect(Math.atan).toHaveLength(1);

    expect(Math.atan(0)).toBe(0);
    expect(Math.atan(-0)).toBe(-0);
    expect(Math.atan(NaN)).toBeNaN();
    expect(Math.atan(-2)).toBeCloseTo(-1.1071487177940904);
    expect(Math.atan(2)).toBeCloseTo(1.1071487177940904);
    expect(Math.atan(Infinity)).toBeCloseTo(Math.PI / 2);
    expect(Math.atan(-Infinity)).toBeCloseTo(-Math.PI / 2);
    expect(Math.atan(0.5)).toBeCloseTo(0.4636476090008061);
});
