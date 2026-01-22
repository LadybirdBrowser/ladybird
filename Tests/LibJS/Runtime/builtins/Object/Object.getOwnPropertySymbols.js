test("use with array", () => {
    let names = Object.getOwnPropertySymbols([1, 2, 3]);
    expect(names).toEqual([]);
});

test("use with object", () => {
    let names = Object.getOwnPropertySymbols({ [Symbol.iterator]: 1, [Symbol.species]: 2 });
    expect(names).toEqual([Symbol.iterator, Symbol.species]);
});

test("use with object with string keys", () => {
    let symbol = Symbol("bar");
    let names = Object.getOwnPropertySymbols({ foo: 1, [symbol]: 2, baz: 3 });
    expect(names).toEqual([symbol]);
});
