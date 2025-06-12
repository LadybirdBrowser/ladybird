test("length is 1", () => {
    expect(isNaN).toHaveLength(1);
});

test("arguments that evaluate to false", () => {
    expect(isNaN(0)).toBeFalse();
    expect(isNaN(42)).toBeFalse();
    expect(isNaN("")).toBeFalse();
    expect(isNaN("0")).toBeFalse();
    expect(isNaN("42")).toBeFalse();
    expect(isNaN(true)).toBeFalse();
    expect(isNaN(false)).toBeFalse();
    expect(isNaN(null)).toBeFalse();
    expect(isNaN([])).toBeFalse();
    expect(isNaN(Infinity)).toBeFalse();
    expect(isNaN(-Infinity)).toBeFalse();
});

test("arguments that evaluate to true", () => {
    expect(isNaN()).toBeTrue();
    expect(isNaN(NaN)).toBeTrue();
    expect(isNaN(undefined)).toBeTrue();
    expect(isNaN("foo")).toBeTrue();
    expect(isNaN({})).toBeTrue();
    expect(isNaN([1, 2, 3])).toBeTrue();
});
