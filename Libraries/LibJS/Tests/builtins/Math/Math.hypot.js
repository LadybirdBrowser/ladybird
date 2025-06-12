test("basic functionality", () => {
    expect(Math.hypot(3, 4)).toBe(5);
    expect(Math.hypot(3, 4, 5)).toBeCloseTo(7.0710678118654755);
    expect(Math.hypot()).toBe(0);
    expect(Math.hypot(NaN)).toBe(NaN);
    expect(Math.hypot(3, 4, "foo")).toBe(NaN);
    expect(Math.hypot(3, 4, "5")).toBeCloseTo(7.0710678118654755);
    expect(Math.hypot(-3)).toBe(3);
});
