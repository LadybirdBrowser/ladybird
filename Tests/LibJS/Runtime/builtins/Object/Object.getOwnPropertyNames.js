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
