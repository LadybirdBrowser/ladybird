const or = (lhs, rhs) => lhs || rhs;

test("booleans", () => {
    // const folded
    expect(true || true).toBeTrue();
    expect(false || false).toBeFalse();
    expect(true || false).toBeTrue();
    expect(false || true).toBeTrue();

    // evaluated
    expect(or(true, true)).toBeTrue();
    expect(or(false, false)).toBeFalse();
    expect(or(true, false)).toBeTrue();
    expect(or(false, true)).toBeTrue();
});

test("strings", () => {
    // const folded
    expect("" || "").toBe("");
    expect("" || false).toBeFalse();
    expect("" || true).toBeTrue();
    expect(false || "").toBe("");
    expect(true || "").toBeTrue();
    expect("foo" || "bar").toBe("foo");
    expect("foo" || false).toBe("foo");
    expect("foo" || true).toBe("foo");
    expect(false || "bar").toBe("bar");
    expect(true || "bar").toBeTrue();

    // evaluated
    expect(or("", "")).toBe("");
    expect(or("", false)).toBeFalse();
    expect(or("", true)).toBeTrue();
    expect(or(false, "")).toBe("");
    expect(or(true, "")).toBeTrue();
    expect(or("foo", "bar")).toBe("foo");
    expect(or("foo", false)).toBe("foo");
    expect(or("foo", true)).toBe("foo");
    expect(or(false, "bar")).toBe("bar");
    expect(or(true, "bar")).toBeTrue();
});

test("numbers", () => {
    // const folded
    expect(false || 1 === 2).toBeFalse();
    expect(true || 1 === 2).toBeTrue();
    expect(0 || false).toBeFalse();
    expect(0 || true).toBeTrue();
    expect(-0 || false).toBeFalse();
    expect(-0 || true).toBeTrue();
    expect(42 || false).toBe(42);
    expect(42 || true).toBe(42);
    expect(false || 0).toBe(0);
    expect(true || 0).toBeTrue();
    expect(false || 42).toBe(42);
    expect(true || 42).toBeTrue();
    expect(NaN || 42).toBe(42);
    expect(Infinity || 42).toBe(Infinity);
    expect(-Infinity || 42).toBe(-Infinity);

    // evaluated
    expect(or(false, 1 === 2)).toBeFalse();
    expect(or(true, 1 === 2)).toBeTrue();
    expect(or(0, false)).toBeFalse();
    expect(or(0, true)).toBeTrue();
    expect(or(-0, false)).toBeFalse();
    expect(or(-0, true)).toBeTrue();
    expect(or(42, false)).toBe(42);
    expect(or(42, true)).toBe(42);
    expect(or(false, 0)).toBe(0);
    expect(or(true, 0)).toBeTrue();
    expect(or(false, 42)).toBe(42);
    expect(or(true, 42)).toBeTrue();
    expect(or(NaN, 42)).toBe(42);
    expect(or(Infinity, 42)).toBe(Infinity);
    expect(or(-Infinity, 42)).toBe(-Infinity);
});

test("objects", () => {
    // const folded
    expect([] || false).toHaveLength(0);
    expect([] || true).toHaveLength(0);
    expect(false || []).toHaveLength(0);
    expect(true || []).toBeTrue();

    // evaluated
    expect(or([], false)).toHaveLength(0);
    expect(or([], true)).toHaveLength(0);
    expect(or(false, [])).toHaveLength(0);
    expect(or(true, [])).toBeTrue();
});

test("null & undefined", () => {
    // const folded
    expect(null || false).toBeFalse();
    expect(null || true).toBeTrue();
    expect(false || null).toBeNull();
    expect(true || null).toBeTrue();
    expect(undefined || false).toBeFalse();
    expect(undefined || true).toBeTrue();
    expect(false || undefined).toBeUndefined();
    expect(true || undefined).toBeTrue();

    // evaluated
    expect(or(null, false)).toBeFalse();
    expect(or(null, true)).toBeTrue();
    expect(or(false, null)).toBeNull();
    expect(or(true, null)).toBeTrue();
    expect(or(undefined, false)).toBeFalse();
    expect(or(undefined, true)).toBeTrue();
    expect(or(false, undefined)).toBeUndefined();
    expect(or(true, undefined)).toBeTrue();
});
