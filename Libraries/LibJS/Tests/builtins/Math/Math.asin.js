test("basic functionality", () => {
    expect(Math.asin).toHaveLength(1);

    expect(Math.asin(0)).toBe(0);
    expect(Math.asin(null)).toBe(0);
    expect(Math.asin("")).toBe(0);
    expect(Math.asin([])).toBe(0);
    expect(Math.asin(1)).toBeCloseTo(1.5707963267948966);
    expect(Math.asin(-1)).toBeCloseTo(-1.5707963267948966);
    expect(Math.asin()).toBeNaN();
    expect(Math.asin(undefined)).toBeNaN();
    expect(Math.asin([1, 2, 3])).toBeNaN();
    expect(Math.asin({})).toBeNaN();
    expect(Math.asin("foo")).toBeNaN();
    expect(Math.asin(NaN)).toBe(NaN);
    expect(Math.asin(-0.0)).toBe(-0.0);
    expect(Math.asin(1.1)).toBe(NaN);
    expect(Math.asin(-1.1)).toBe(NaN);
});
