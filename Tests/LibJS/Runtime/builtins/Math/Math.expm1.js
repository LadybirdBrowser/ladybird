test("basic functionality", () => {
    expect(Math.expm1).toHaveLength(1);

    expect(Math.expm1(0)).toBe(0);
    expect(Math.expm1(-2)).toBeCloseTo(-0.864664);
    expect(Math.expm1(-1)).toBeCloseTo(-0.63212);
    expect(Math.expm1(1)).toBeCloseTo(1.718281);
    expect(Math.expm1(2)).toBeCloseTo(6.389056);

    expect(Math.expm1()).toBeNaN();
    expect(Math.expm1(undefined)).toBeNaN();
    expect(Math.expm1("foo")).toBeNaN();
});
