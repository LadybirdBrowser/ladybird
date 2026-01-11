const and = (lhs, rhs) => lhs && rhs;

test("booleans", () => {
    // const folded
    expect(true && true).toBeTrue();
    expect(false && false).toBeFalse();
    expect(true && false).toBeFalse();
    expect(false && true).toBeFalse();

    // evaluated
    expect(and(true, true)).toBeTrue();
    expect(and(false, false)).toBeFalse();
    expect(and(true, false)).toBeFalse();
    expect(and(false, true)).toBeFalse();
});

test("strings", () => {
    // const folded
    expect("" && "").toBe("");
    expect("" && false).toBe("");
    expect("" && true).toBe("");
    expect(false && "").toBeFalse();
    expect(true && "").toBe("");
    expect("foo" && "bar").toBe("bar");
    expect("foo" && false).toBeFalse();
    expect("foo" && true).toBeTrue();
    expect(false && "bar").toBeFalse();
    expect(true && "bar").toBe("bar");

    // evaluated
    expect(and("", "")).toBe("");
    expect(and("", false)).toBe("");
    expect(and("", true)).toBe("");
    expect(and(false, "")).toBeFalse();
    expect(and(true, "")).toBe("");
    expect(and("foo", "bar")).toBe("bar");
    expect(and("foo", false)).toBeFalse();
    expect(and("foo", true)).toBeTrue();
    expect(and(false, "bar")).toBeFalse();
    expect(and(true, "bar")).toBe("bar");
});

test("numbers", () => {
    // const folded
    expect(false && 1 === 2).toBeFalse();
    expect(true && 1 === 2).toBeFalse();
    expect(0 && false).toBe(0);
    expect(0 && true).toBe(0);
    expect(-0 && false).toBe(-0);
    expect(-0 && true).toBe(-0);
    expect(42 && false).toBeFalse();
    expect(42 && true).toBeTrue();
    expect(false && 0).toBeFalse();
    expect(true && 0).toBe(0);
    expect(false && 42).toBeFalse();
    expect(true && 42).toBe(42);
    expect(NaN && 42).toBe(NaN);
    expect(Infinity && 42).toBe(42);
    expect(-Infinity && 42).toBe(42);

    // evaluated
    expect(and(false, 1 === 2)).toBeFalse();
    expect(and(true, 1 === 2)).toBeFalse();
    expect(and(0, false)).toBe(0);
    expect(and(0, true)).toBe(0);
    expect(and(-0, false)).toBe(-0);
    expect(and(-0, true)).toBe(-0);
    expect(and(42, false)).toBeFalse();
    expect(and(42, true)).toBeTrue();
    expect(and(false, 0)).toBeFalse();
    expect(and(true, 0)).toBe(0);
    expect(and(false, 42)).toBeFalse();
    expect(and(true, 42)).toBe(42);
    expect(and(NaN, 42)).toBe(NaN);
    expect(and(Infinity, 42)).toBe(42);
    expect(and(-Infinity, 42)).toBe(42);
});

test("objects", () => {
    // const folded
    expect([] && false).toBeFalse();
    expect([] && true).toBeTrue();
    expect(false && []).toBeFalse();
    expect(true && []).toHaveLength(0);

    // evaluated
    expect(and([], false)).toBeFalse();
    expect(and([], true)).toBeTrue();
    expect(and(false, [])).toBeFalse();
    expect(and(true, [])).toHaveLength(0);
});

test("null & undefined", () => {
    // const folded
    expect(null && false).toBeNull();
    expect(null && true).toBeNull();
    expect(false && null).toBeFalse();
    expect(true && null).toBeNull();
    expect(undefined && false).toBeUndefined();
    expect(undefined && true).toBeUndefined();
    expect(false && undefined).toBeFalse();
    expect(true && undefined).toBeUndefined();

    // evaluated
    expect(and(null, false)).toBeNull();
    expect(and(null, true)).toBeNull();
    expect(and(false, null)).toBeFalse();
    expect(and(true, null)).toBeNull();
    expect(and(undefined, false)).toBeUndefined();
    expect(and(undefined, true)).toBeUndefined();
    expect(and(false, undefined)).toBeFalse();
    expect(and(true, undefined)).toBeUndefined();
});
