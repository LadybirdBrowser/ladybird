test("basic functionality", () => {
    expect(Number.isNaN).toHaveLength(1);
    expect(Number.isNaN).not.toBe(isNaN);

    expect(Number.isNaN(0)).toBeFalse();
    expect(Number.isNaN(42)).toBeFalse();
    expect(Number.isNaN("")).toBeFalse();
    expect(Number.isNaN("0")).toBeFalse();
    expect(Number.isNaN("42")).toBeFalse();
    expect(Number.isNaN(true)).toBeFalse();
    expect(Number.isNaN(false)).toBeFalse();
    expect(Number.isNaN(null)).toBeFalse();
    expect(Number.isNaN([])).toBeFalse();
    expect(Number.isNaN(Infinity)).toBeFalse();
    expect(Number.isNaN(-Infinity)).toBeFalse();
    expect(Number.isNaN()).toBeFalse();
    expect(Number.isNaN(undefined)).toBeFalse();
    expect(Number.isNaN("foo")).toBeFalse();
    expect(Number.isNaN({})).toBeFalse();
    expect(Number.isNaN([1, 2, 3])).toBeFalse();

    expect(Number.isNaN(NaN)).toBeTrue();
    expect(Number.isNaN(Number.NaN)).toBeTrue();
    expect(Number.isNaN(0 / 0)).toBeTrue();
});
