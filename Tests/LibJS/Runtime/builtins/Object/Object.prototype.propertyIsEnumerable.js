test("basic functionality", () => {
    var o = {};

    o.foo = 1;
    expect(o.propertyIsEnumerable("foo")).toBeTrue();
    expect(o.propertyIsEnumerable("bar")).toBeFalse();
    expect(o.propertyIsEnumerable()).toBeFalse();
    expect(o.propertyIsEnumerable(undefined)).toBeFalse();

    o.undefined = 2;
    expect(o.propertyIsEnumerable()).toBeTrue();
    expect(o.propertyIsEnumerable(undefined)).toBeTrue();

    expect(globalThis.propertyIsEnumerable("globalThis")).toBeFalse();
});
