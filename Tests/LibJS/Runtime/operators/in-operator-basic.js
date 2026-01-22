test("in operator with objects", () => {
    const sym = Symbol();
    const o = { foo: "bar", bar: undefined, [sym]: "qux" };
    expect("" in o).toBeFalse();
    expect("foo" in o).toBeTrue();
    expect("bar" in o).toBeTrue();
    expect("baz" in o).toBeFalse();
    expect("toString" in o).toBeTrue();
    expect(sym in o).toBeTrue();
});

test("in operator with arrays", () => {
    const a = ["hello", "friends"];
    expect(0 in a).toBeTrue();
    expect(1 in a).toBeTrue();
    expect(2 in a).toBeFalse();
    expect("0" in a).toBeTrue();
    expect("hello" in a).toBeFalse();
    expect("friends" in a).toBeFalse();
    expect("length" in a).toBeTrue();
});

test("in operator with string object", () => {
    const s = new String("foo");
    expect("length" in s).toBeTrue();
});

test("error when used with primitives", () => {
    ["foo", 123, null, undefined].forEach(value => {
        expect(() => {
            "prop" in value;
        }).toThrowWithMessage(TypeError, "'in' operator must be used on an object");
    });
});
