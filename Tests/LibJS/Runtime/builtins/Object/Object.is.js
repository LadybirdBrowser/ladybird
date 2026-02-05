test("length", () => {
    expect(Object.is).toHaveLength(2);
});

test("arguments that evaluate to true", () => {
    let a = [1, 2, 3];
    let o = { foo: "bar" };

    expect(Object.is("", "")).toBeTrue();
    expect(Object.is("foo", "foo")).toBeTrue();
    expect(Object.is(0, 0)).toBeTrue();
    expect(Object.is(+0, +0)).toBeTrue();
    expect(Object.is(-0, -0)).toBeTrue();
    expect(Object.is(1.23, 1.23)).toBeTrue();
    expect(Object.is(42, 42)).toBeTrue();
    expect(Object.is(NaN, NaN)).toBeTrue();
    expect(Object.is(Infinity, Infinity)).toBeTrue();
    expect(Object.is(+Infinity, +Infinity)).toBeTrue();
    expect(Object.is(-Infinity, -Infinity)).toBeTrue();
    expect(Object.is(true, true)).toBeTrue();
    expect(Object.is(false, false)).toBeTrue();
    expect(Object.is(null, null)).toBeTrue();
    expect(Object.is(undefined, undefined)).toBeTrue();
    expect(Object.is(undefined)).toBeTrue();
    expect(Object.is()).toBeTrue();
    expect(Object.is(a, a)).toBeTrue();
    expect(Object.is(o, o)).toBeTrue();
});

test("arguments that evaluate to false", () => {
    let a = [1, 2, 3];
    let o = { foo: "bar" };

    expect(Object.is("test")).toBeFalse();
    expect(Object.is("foo", "bar")).toBeFalse();
    expect(Object.is(1, "1")).toBeFalse();
    expect(Object.is(+0, -0)).toBeFalse();
    expect(Object.is(-0, +0)).toBeFalse();
    expect(Object.is(42, 24)).toBeFalse();
    expect(Object.is(Infinity, -Infinity)).toBeFalse();
    expect(Object.is(-Infinity, +Infinity)).toBeFalse();
    expect(Object.is(true, false)).toBeFalse();
    expect(Object.is(false, true)).toBeFalse();
    expect(Object.is(undefined, null)).toBeFalse();
    expect(Object.is(null, undefined)).toBeFalse();
    expect(Object.is([], [])).toBeFalse();
    expect(Object.is(a, [1, 2, 3])).toBeFalse();
    expect(Object.is([1, 2, 3], a)).toBeFalse();
    expect(Object.is({}, {})).toBeFalse();
    expect(Object.is(o, { foo: "bar" })).toBeFalse();
    expect(Object.is({ foo: "bar" }, o)).toBeFalse();
    expect(Object.is(a, o)).toBeFalse();
    expect(Object.is(o, a)).toBeFalse();
});
