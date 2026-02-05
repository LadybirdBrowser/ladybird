test("basic functionality", () => {
    expect(Math.trunc).toHaveLength(1);

    expect(Math.trunc(13.37)).toBe(13);
    expect(Math.trunc(42.84)).toBe(42);
    expect(Math.trunc(0.123)).toBe(0);
    expect(Math.trunc(-0.123)).toBe(-0);

    expect(Math.trunc(NaN)).toBeNaN();
    expect(Math.trunc("foo")).toBeNaN();
    expect(Math.trunc()).toBeNaN();
});
