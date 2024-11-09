test("basic functionality", () => {
    expect(Number.isFinite).toHaveLength(1);
    expect(Number.isFinite).not.toBe(isFinite);

    expect(Number.isFinite(0)).toBeTrue();
    expect(Number.isFinite(1.23)).toBeTrue();
    expect(Number.isFinite(42)).toBeTrue();

    expect(Number.isFinite("")).toBeFalse();
    expect(Number.isFinite("0")).toBeFalse();
    expect(Number.isFinite("42")).toBeFalse();
    expect(Number.isFinite(true)).toBeFalse();
    expect(Number.isFinite(false)).toBeFalse();
    expect(Number.isFinite(null)).toBeFalse();
    expect(Number.isFinite([])).toBeFalse();
    expect(Number.isFinite()).toBeFalse();
    expect(Number.isFinite(NaN)).toBeFalse();
    expect(Number.isFinite(undefined)).toBeFalse();
    expect(Number.isFinite(Infinity)).toBeFalse();
    expect(Number.isFinite(-Infinity)).toBeFalse();
    expect(Number.isFinite("foo")).toBeFalse();
    expect(Number.isFinite({})).toBeFalse();
    expect(Number.isFinite([1, 2, 3])).toBeFalse();
});
