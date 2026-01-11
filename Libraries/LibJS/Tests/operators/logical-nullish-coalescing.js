const coalesce = (lhs, rhs) => lhs ?? rhs;

test("booleans", () => {
    // const folded
    expect(true ?? true).toBeTrue();
    expect(false ?? false).toBeFalse();
    expect(true ?? false).toBeTrue();
    expect(false ?? true).toBeFalse();

    // evaluated
    expect(coalesce(true, true)).toBeTrue();
    expect(coalesce(false, false)).toBeFalse();
    expect(coalesce(true, false)).toBeTrue();
    expect(coalesce(false, true)).toBeFalse();
});

test("strings", () => {
    // const folded
    expect("" ?? "").toBe("");
    expect("" ?? false).toBe("");
    expect("" ?? true).toBe("");
    expect(false ?? "").toBeFalse();
    expect(true ?? "").toBeTrue();
    expect("foo" ?? "bar").toBe("foo");
    expect("foo" ?? false).toBe("foo");
    expect("foo" ?? true).toBe("foo");
    expect(false ?? "bar").toBeFalse();
    expect(true ?? "bar").toBeTrue();

    // evaluated
    expect(coalesce("", "")).toBe("");
    expect(coalesce("", false)).toBe("");
    expect(coalesce("", true)).toBe("");
    expect(coalesce(false, "")).toBeFalse();
    expect(coalesce(true, "")).toBeTrue();
    expect(coalesce("foo", "bar")).toBe("foo");
    expect(coalesce("foo", false)).toBe("foo");
    expect(coalesce("foo", true)).toBe("foo");
    expect(coalesce(false, "bar")).toBeFalse();
    expect(coalesce(true, "bar")).toBeTrue();
});

test("numbers", () => {
    // const folded
    expect(false ?? 1 === 2).toBeFalse();
    expect(true ?? 1 === 2).toBeTrue();
    expect(0 ?? false).toBe(0);
    expect(0 ?? true).toBe(0);
    expect(42 ?? false).toBe(42);
    expect(42 ?? true).toBe(42);
    expect(false ?? 0).toBeFalse();
    expect(true ?? 0).toBeTrue();
    expect(false ?? 42).toBeFalse();
    expect(true ?? 42).toBeTrue();

    // evaluated
    expect(coalesce(false, 1 === 2)).toBeFalse();
    expect(coalesce(true, 1 === 2)).toBeTrue();
    expect(coalesce(0, false)).toBe(0);
    expect(coalesce(0, true)).toBe(0);
    expect(coalesce(42, false)).toBe(42);
    expect(coalesce(42, true)).toBe(42);
    expect(coalesce(false, 0)).toBeFalse();
    expect(coalesce(true, 0)).toBeTrue();
    expect(coalesce(false, 42)).toBeFalse();
    expect(coalesce(true, 42)).toBeTrue();
});

test("objects", () => {
    // const folded
    expect([] ?? false).toHaveLength(0);
    expect([] ?? true).toHaveLength(0);
    expect(false ?? []).toBeFalse();
    expect(true ?? []).toBeTrue();

    // evaluated
    expect(coalesce([], false)).toHaveLength(0);
    expect(coalesce([], true)).toHaveLength(0);
    expect(coalesce(false, [])).toBeFalse();
    expect(coalesce(true, [])).toBeTrue();
});

test("null & undefined", () => {
    // const folded
    expect(null ?? false).toBeFalse();
    expect(null ?? true).toBeTrue();
    expect(false ?? null).toBeFalse();
    expect(true ?? null).toBeTrue();
    expect(undefined ?? false).toBeFalse();
    expect(undefined ?? true).toBeTrue();
    expect(false ?? undefined).toBeFalse();
    expect(true ?? undefined).toBeTrue();

    // evaluated
    expect(coalesce(null, false)).toBeFalse();
    expect(coalesce(null, true)).toBeTrue();
    expect(coalesce(false, null)).toBeFalse();
    expect(coalesce(true, null)).toBeTrue();
    expect(coalesce(undefined, false)).toBeFalse();
    expect(coalesce(undefined, true)).toBeTrue();
    expect(coalesce(false, undefined)).toBeFalse();
    expect(coalesce(true, undefined)).toBeTrue();
});
