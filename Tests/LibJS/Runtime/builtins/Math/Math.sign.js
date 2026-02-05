function isPositiveZero(value) {
    return value === 0 && 1 / value === Infinity;
}

function isNegativeZero(value) {
    return value === 0 && 1 / value === -Infinity;
}

test("basic functionality", () => {
    expect(Math.sign).toHaveLength(1);

    expect(Math.sign(0.0001)).toBe(1);
    expect(Math.sign(1)).toBe(1);
    expect(Math.sign(42)).toBe(1);
    expect(Math.sign(Infinity)).toBe(1);
    expect(isPositiveZero(Math.sign(0))).toBeTrue();
    expect(isPositiveZero(Math.sign(null))).toBeTrue();
    expect(isPositiveZero(Math.sign(""))).toBeTrue();
    expect(isPositiveZero(Math.sign([]))).toBeTrue();

    expect(Math.sign(-0.0001)).toBe(-1);
    expect(Math.sign(-1)).toBe(-1);
    expect(Math.sign(-42)).toBe(-1);
    expect(Math.sign(-Infinity)).toBe(-1);
    expect(isNegativeZero(Math.sign(-0))).toBeTrue();
    expect(isNegativeZero(Math.sign(-null))).toBeTrue();
    expect(isNegativeZero(Math.sign(-""))).toBeTrue();
    expect(isNegativeZero(Math.sign(-[]))).toBeTrue();

    expect(Math.sign()).toBeNaN();
    expect(Math.sign(undefined)).toBeNaN();
    expect(Math.sign([1, 2, 3])).toBeNaN();
    expect(Math.sign({})).toBeNaN();
    expect(Math.sign(NaN)).toBeNaN();
    expect(Math.sign("foo")).toBeNaN();
});
