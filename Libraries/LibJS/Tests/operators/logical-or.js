test("booleans", () => {
    expect(true || true).toBeTrue();
    expect(false || false).toBeFalse();
    expect(true || false).toBeTrue();
    expect(false || true).toBeTrue();
});

test("strings", () => {
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
});

test("numbers", () => {
    expect(false || 1 === 2).toBeFalse();
    expect(true || 1 === 2).toBeTrue();
    expect(0 || false).toBeFalse();
    expect(0 || true).toBeTrue();
    expect(42 || false).toBe(42);
    expect(42 || true).toBe(42);
    expect(false || 0).toBe(0);
    expect(true || 0).toBeTrue();
    expect(false || 42).toBe(42);
    expect(true || 42).toBeTrue();
});

test("objects", () => {
    expect([] || false).toHaveLength(0);
    expect([] || true).toHaveLength(0);
    expect(false || []).toHaveLength(0);
    expect(true || []).toBeTrue();
});

test("null & undefined", () => {
    expect(null || false).toBeFalse();
    expect(null || true).toBeTrue();
    expect(false || null).toBeNull();
    expect(true || null).toBeTrue();
    expect(undefined || false).toBeFalse();
    expect(undefined || true).toBeTrue();
    expect(false || undefined).toBeUndefined();
    expect(true || undefined).toBeTrue();
});
