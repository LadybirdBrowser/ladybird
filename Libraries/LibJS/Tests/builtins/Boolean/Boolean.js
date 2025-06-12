test("constructor properties", () => {
    expect(Boolean).toHaveLength(1);
    expect(Boolean.name).toBe("Boolean");
});

test("typeof", () => {
    expect(typeof new Boolean()).toBe("object");
    expect(typeof Boolean()).toBe("boolean");
    expect(typeof Boolean(true)).toBe("boolean");
});

test("basic functionality", () => {
    var foo = new Boolean(true);
    var bar = new Boolean(true);

    expect(foo).not.toBe(bar);
    expect(foo.valueOf()).toBe(bar.valueOf());

    expect(Boolean()).toBeFalse();
    expect(Boolean(false)).toBeFalse();
    expect(Boolean(null)).toBeFalse();
    expect(Boolean(undefined)).toBeFalse();
    expect(Boolean(NaN)).toBeFalse();
    expect(Boolean("")).toBeFalse();
    expect(Boolean(0.0)).toBeFalse();
    expect(Boolean(-0.0)).toBeFalse();
    expect(Boolean(true)).toBeTrue();
    expect(Boolean("0")).toBeTrue();
    expect(Boolean({})).toBeTrue();
    expect(Boolean([])).toBeTrue();
    expect(Boolean(1)).toBeTrue();
});
