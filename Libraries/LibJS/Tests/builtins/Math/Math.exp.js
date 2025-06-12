test("basic functionality", () => {
    expect(Math.exp).toHaveLength(1);

    expect(Math.exp(0)).toBe(1);
    expect(Math.exp(-2)).toBeCloseTo(0.135335);
    expect(Math.exp(-1)).toBeCloseTo(0.367879);
    expect(Math.exp(1)).toBeCloseTo(2.718281);
    expect(Math.exp(2)).toBeCloseTo(7.389056);

    expect(Math.exp()).toBeNaN();
    expect(Math.exp(undefined)).toBeNaN();
    expect(Math.exp("foo")).toBeNaN();
});
