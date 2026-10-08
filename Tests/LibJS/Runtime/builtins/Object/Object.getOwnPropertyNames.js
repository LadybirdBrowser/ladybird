test("use with array", () => {
    let names = Object.getOwnPropertyNames([1, 2, 3]);
    expect(names).toEqual(["0", "1", "2", "length"]);
});

test("use with object", () => {
    let names = Object.getOwnPropertyNames({ foo: 1, bar: 2, baz: 3 });
    expect(names).toEqual(["foo", "bar", "baz"]);
});

test("use with object with symbol keys", () => {
    let names = Object.getOwnPropertyNames({ foo: 1, [Symbol("bar")]: 2, baz: 3 });
    expect(names).toEqual(["foo", "baz"]);
});

test("use with String object", () => {
    let names = Object.getOwnPropertyNames(new String("foo"));
    expect(names).toEqual(["0", "1", "2", "length"]);
});

test("names stay right as objects of the same shape change", () => {
    const make = () => ({ b: 1, a: 2, [Symbol("s")]: 3 });
    const object = make();
    Object.defineProperty(object, "hidden", { value: 1, enumerable: false });
    expect(Object.getOwnPropertyNames(object)).toEqual(["b", "a", "hidden"]);
    expect(Object.getOwnPropertyNames(make())).toEqual(["b", "a"]);
    delete object.a;
    expect(Object.getOwnPropertyNames(object)).toEqual(["b", "hidden"]);
    object[2] = 0;
    expect(Object.getOwnPropertyNames(object)).toEqual(["2", "b", "hidden"]);

    function f(a, b) {}
    expect(Object.getOwnPropertyNames(f)).toContain("prototype");
    expect(Object.getOwnPropertyNames([1])).toEqual(["0", "length"]);
    expect(Object.getOwnPropertyNames(Math)).toContain("abs");
});
