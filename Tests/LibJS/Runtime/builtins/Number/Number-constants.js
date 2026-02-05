test("basic functionality", () => {
    expect(Number.EPSILON).toBe(2 ** -52);
    expect(Number.EPSILON).toBeGreaterThan(0);
    expect(Number.MAX_SAFE_INTEGER).toBe(2 ** 53 - 1);
    expect(Number.MAX_SAFE_INTEGER + 1).toBe(Number.MAX_SAFE_INTEGER + 2);
    expect(Number.MIN_SAFE_INTEGER).toBe(-(2 ** 53 - 1));
    expect(Number.MIN_SAFE_INTEGER - 1).toBe(Number.MIN_SAFE_INTEGER - 2);
    expect(Number.POSITIVE_INFINITY).toBe(Infinity);
    expect(Number.NEGATIVE_INFINITY).toBe(-Infinity);
    expect(Number.NaN).toBeNaN();
    expect(Number.MIN_VALUE).toBeGreaterThan(0);
    expect(Number.MIN_VALUE / 2.0).toBe(0);
});
