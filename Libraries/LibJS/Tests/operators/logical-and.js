test("booleans", () => {
    expect(true && true).toBeTrue();
    expect(false && false).toBeFalse();
    expect(true && false).toBeFalse();
    expect(false && true).toBeFalse();
});

test("strings", () => {
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
});

test("numbers", () => {
    expect(false && 1 === 2).toBeFalse();
    expect(true && 1 === 2).toBeFalse();
    expect(0 && false).toBe(0);
    expect(0 && true).toBe(0);
    expect(42 && false).toBeFalse();
    expect(42 && true).toBeTrue();
    expect(false && 0).toBeFalse();
    expect(true && 0).toBe(0);
    expect(false && 42).toBeFalse();
    expect(true && 42).toBe(42);
});

test("objects", () => {
    expect([] && false).toBeFalse();
    expect([] && true).toBeTrue();
    expect(false && []).toBeFalse();
    expect(true && []).toHaveLength(0);
});

test("null & undefined", () => {
    expect(null && false).toBeNull();
    expect(null && true).toBeNull();
    expect(false && null).toBeFalse();
    expect(true && null).toBeNull();
    expect(undefined && false).toBeUndefined();
    expect(undefined && true).toBeUndefined();
    expect(false && undefined).toBeFalse();
    expect(true && undefined).toBeUndefined();
});
