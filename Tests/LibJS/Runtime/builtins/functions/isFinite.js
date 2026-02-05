test("basic functionality", () => {
    expect(isFinite).toHaveLength(1);

    expect(isFinite(0)).toBeTrue();
    expect(isFinite(1.23)).toBeTrue();
    expect(isFinite(42)).toBeTrue();
    expect(isFinite("")).toBeTrue();
    expect(isFinite("0")).toBeTrue();
    expect(isFinite("42")).toBeTrue();
    expect(isFinite(true)).toBeTrue();
    expect(isFinite(false)).toBeTrue();
    expect(isFinite(null)).toBeTrue();
    expect(isFinite([])).toBeTrue();

    expect(isFinite()).toBeFalse();
    expect(isFinite(NaN)).toBeFalse();
    expect(isFinite(undefined)).toBeFalse();
    expect(isFinite(Infinity)).toBeFalse();
    expect(isFinite(-Infinity)).toBeFalse();
    expect(isFinite("foo")).toBeFalse();
    expect(isFinite({})).toBeFalse();
    expect(isFinite([1, 2, 3])).toBeFalse();
});
