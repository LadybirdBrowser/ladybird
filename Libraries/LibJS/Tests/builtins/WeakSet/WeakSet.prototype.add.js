test("basic functionality", () => {
    expect(WeakSet.prototype.add).toHaveLength(1);

    const weakSet = new WeakSet([{ a: 1 }, { a: 2 }, { a: 3 }, Symbol("foo")]);
    expect(weakSet.add({ a: 4 })).toBe(weakSet);
    expect(weakSet.add({ a: 1 })).toBe(weakSet);
    expect(weakSet.add(Symbol("bar"))).toBe(weakSet);
});

test("invalid values", () => {
    const weakSet = new WeakSet();
    [-100, Infinity, NaN, "hello", 152n].forEach(value => {
        expect(() => {
            weakSet.add(value);
        }).toThrowWithMessage(TypeError, "cannot be held weakly");
    });
});

test.xfail("automatic removal of garbage-collected values", () => {
    const weakSet = new WeakSet();
    const objectItem = { a: 1 };

    expect(weakSet.add(objectItem)).toBe(weakSet);
    expect(getWeakSetSize(weakSet)).toBe(1);

    markAsGarbage("objectItem");
    gc();

    expect(getWeakSetSize(weakSet)).toBe(0);

    const symbolItem = Symbol("foo");

    expect(weakSet.add(symbolItem)).toBe(weakSet);
    expect(getWeakSetSize(weakSet)).toBe(1);

    markAsGarbage("symbolItem");
    gc();

    expect(getWeakSetSize(weakSet)).toBe(0);
});
