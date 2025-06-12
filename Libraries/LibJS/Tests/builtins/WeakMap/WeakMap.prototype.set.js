test("basic functionality", () => {
    expect(WeakMap.prototype.set).toHaveLength(2);

    const weakMap = new WeakMap([
        [{ a: 1 }, 1],
        [{ a: 2 }, 2],
        [{ a: 3 }, 3],
        [Symbol("foo"), "bar"],
    ]);
    expect(weakMap.set({ a: 4 }, 4)).toBe(weakMap);
    expect(weakMap.set({ a: 1 }, 2)).toBe(weakMap);
    expect(weakMap.set(Symbol("hello"), "friends")).toBe(weakMap);
});

test("invalid values", () => {
    const weakMap = new WeakMap();
    [-100, Infinity, NaN, "hello", 152n].forEach(value => {
        expect(() => {
            weakMap.set(value, value);
        }).toThrowWithMessage(TypeError, "cannot be held weakly");
    });
});

test.xfail("automatic removal of garbage-collected values", () => {
    const weakMap = new WeakMap();
    const objectKey = { e: 3 };

    expect(weakMap.set(objectKey, 1)).toBe(weakMap);
    expect(getWeakMapSize(weakMap)).toBe(1);

    markAsGarbage("objectKey");
    gc();

    expect(getWeakMapSize(weakMap)).toBe(0);

    const symbolKey = Symbol("foo");

    expect(weakMap.set(symbolKey, "bar")).toBe(weakMap);
    expect(getWeakMapSize(weakMap)).toBe(1);

    markAsGarbage("symbolKey");
    gc();

    expect(getWeakMapSize(weakMap)).toBe(0);
});
