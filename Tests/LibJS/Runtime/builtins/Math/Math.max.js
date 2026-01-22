test("basic functionality", () => {
    expect(Math.max).toHaveLength(2);

    expect(Math.max()).toBe(-Infinity);
    expect(Math.max(1)).toBe(1);
    expect(Math.max(2, 1)).toBe(2);
    expect(Math.max(1, 2, 3)).toBe(3);
    expect(Math.max(-0, 0)).toBe(0);
    expect(Math.max(0, -0)).toBe(0);
    expect(Math.max(NaN)).toBeNaN();
    expect(Math.max("String", 1)).toBeNaN();
});
