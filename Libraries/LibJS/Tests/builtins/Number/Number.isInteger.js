test("basic functionality", () => {
    expect(Number.isInteger).toHaveLength(1);

    expect(Number.isInteger(0)).toBeTrue();
    expect(Number.isInteger(42)).toBeTrue();
    expect(Number.isInteger(-10000)).toBeTrue();
    expect(Number.isInteger(5)).toBeTrue();
    expect(Number.isInteger(5.0)).toBeTrue();
    expect(Number.isInteger(5 + 1 / 10000000000000000)).toBeTrue();
    expect(Number.isInteger(+2147483647 + 1)).toBeTrue();
    expect(Number.isInteger(-2147483648 - 1)).toBeTrue();
    expect(Number.isInteger(99999999999999999999999999999999999)).toBeTrue();

    expect(Number.isInteger(5 + 1 / 1000000000000000)).toBeFalse();
    expect(Number.isInteger(1.23)).toBeFalse();
    expect(Number.isInteger("")).toBeFalse();
    expect(Number.isInteger("0")).toBeFalse();
    expect(Number.isInteger("42")).toBeFalse();
    expect(Number.isInteger(true)).toBeFalse();
    expect(Number.isInteger(false)).toBeFalse();
    expect(Number.isInteger(null)).toBeFalse();
    expect(Number.isInteger([])).toBeFalse();
    expect(Number.isInteger(Infinity)).toBeFalse();
    expect(Number.isInteger(-Infinity)).toBeFalse();
    expect(Number.isInteger(NaN)).toBeFalse();
    expect(Number.isInteger()).toBeFalse();
    expect(Number.isInteger(undefined)).toBeFalse();
    expect(Number.isInteger("foo")).toBeFalse();
    expect(Number.isInteger({})).toBeFalse();
    expect(Number.isInteger([1, 2, 3])).toBeFalse();
});
