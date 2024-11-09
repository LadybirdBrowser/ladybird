test("basic functionality", () => {
    expect(Math.floor).toHaveLength(1);

    expect(Math.floor(0.95)).toBe(0);
    expect(Math.floor(4)).toBe(4);
    expect(Math.floor(7.004)).toBe(7);
    expect(Math.floor(-0.95)).toBe(-1);
    expect(Math.floor(-4)).toBe(-4);
    expect(Math.floor(-7.004)).toBe(-8);

    expect(Math.floor()).toBeNaN();
    expect(Math.floor(NaN)).toBeNaN();
});
