test("basic functionality", () => {
    expect(Number.isSafeInteger).toHaveLength(1);

    expect(Number.isSafeInteger(0)).toBeTrue();
    expect(Number.isSafeInteger(1)).toBeTrue();
    expect(Number.isSafeInteger(2.0)).toBeTrue();
    expect(Number.isSafeInteger(42)).toBeTrue();
    expect(Number.isSafeInteger(Number.MAX_SAFE_INTEGER)).toBeTrue();
    expect(Number.isSafeInteger(Number.MIN_SAFE_INTEGER)).toBeTrue();

    expect(Number.isSafeInteger()).toBeFalse();
    expect(Number.isSafeInteger("1")).toBeFalse();
    expect(Number.isSafeInteger(2.1)).toBeFalse();
    expect(Number.isSafeInteger(42.42)).toBeFalse();
    expect(Number.isSafeInteger("")).toBeFalse();
    expect(Number.isSafeInteger([])).toBeFalse();
    expect(Number.isSafeInteger(null)).toBeFalse();
    expect(Number.isSafeInteger(undefined)).toBeFalse();
    expect(Number.isSafeInteger(NaN)).toBeFalse();
    expect(Number.isSafeInteger(Infinity)).toBeFalse();
    expect(Number.isSafeInteger(-Infinity)).toBeFalse();
    expect(Number.isSafeInteger(Number.MAX_SAFE_INTEGER + 1)).toBeFalse();
    expect(Number.isSafeInteger(Number.MIN_SAFE_INTEGER - 1)).toBeFalse();
});
