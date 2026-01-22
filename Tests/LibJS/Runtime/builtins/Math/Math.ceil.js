test("basic functionality", () => {
    expect(Math.ceil).toHaveLength(1);

    expect(Math.ceil(0.95)).toBe(1);
    expect(Math.ceil(4)).toBe(4);
    expect(Math.ceil(7.004)).toBe(8);
    expect(Math.ceil(-0.95)).toBe(-0);
    expect(Math.ceil(-4)).toBe(-4);
    expect(Math.ceil(-7.004)).toBe(-7);

    expect(Math.ceil()).toBeNaN();
    expect(Math.ceil(NaN)).toBeNaN();
});
