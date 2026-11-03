test("basic functionality", () => {
    expect(Math.hypot(3, 4)).toBe(5);
    expect(Math.hypot(3, 4, 5)).toBeCloseTo(7.0710678118654755);
    expect(Math.hypot()).toBe(0);
    expect(Math.hypot(NaN)).toBe(NaN);
    expect(Math.hypot(3, 4, "foo")).toBe(NaN);
    expect(Math.hypot(3, 4, "5")).toBeCloseTo(7.0710678118654755);
    expect(Math.hypot(-3)).toBe(3);
});

test("does not overflow or underflow", () => {
    expect(Math.hypot(1e200, 1e200)).toBe(1.414213562373095e200);
    expect(Math.hypot(1e-200, 1e-200)).toBe(1.414213562373095e-200);
    expect(Math.hypot(Number.MAX_VALUE, Number.MAX_VALUE)).toBe(Infinity);
    expect(Math.hypot(Number.MIN_VALUE, -Number.MIN_VALUE)).toBe(Number.MIN_VALUE);
    expect(Math.hypot(1e200, 0, 0, 1e200)).toBe(1.414213562373095e200);
    expect(Math.hypot(1e200, 1e200, 1e200)).toBe(1.7320508075688773e200);
});

test("is correctly rounded", () => {
    expect(Math.hypot(0.8776669492836708, 0.2058533185844357)).toBe(0.9014848099869076);
    expect(Math.hypot(1, 1e-9)).toBe(1);
});

test("infinity wins over NaN, and NaN over everything else", () => {
    expect(Math.hypot(NaN, -Infinity)).toBe(Infinity);
    expect(Math.hypot(-Infinity, NaN)).toBe(Infinity);
    expect(Math.hypot(3, NaN, 4)).toBe(NaN);
    expect(Math.hypot(Number.MAX_VALUE, Number.MAX_VALUE, NaN)).toBe(NaN);
    expect(Math.hypot(-0, -0)).toBe(0);
    expect(Object.is(Math.hypot(-0), 0)).toBeTrue();
});
