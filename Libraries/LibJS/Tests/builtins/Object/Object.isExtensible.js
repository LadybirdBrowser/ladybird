test("basic functionality", () => {
    expect(Object.isExtensible).toHaveLength(1);

    expect(Object.isExtensible()).toBeFalse();
    expect(Object.isExtensible(undefined)).toBeFalse();
    expect(Object.isExtensible(null)).toBeFalse();
    expect(Object.isExtensible(true)).toBeFalse();
    expect(Object.isExtensible(6)).toBeFalse();
    expect(Object.isExtensible("test")).toBeFalse();

    let s = Symbol();
    expect(Object.isExtensible(s)).toBeFalse();

    let o = { foo: "foo" };
    expect(Object.isExtensible(o)).toBeTrue();
    Object.preventExtensions(o);
    expect(Object.isExtensible(o)).toBeFalse();
});
