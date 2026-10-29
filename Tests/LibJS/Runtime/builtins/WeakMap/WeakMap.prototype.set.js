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
    [-100, Infinity, NaN, "hello", 152n].forEach(key => {
        const suffix = typeof key === "bigint" ? "n" : "";

        expect(() => {
            weakMap.set(key, 1);
        }).toThrowWithMessage(TypeError, `${key}${suffix} cannot be held weakly`);
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

test("a value does not keep its own key alive", () => {
    // Fill the map in a function of its own, so that the JIT leaves none of the keys in this frame.
    const weakMap = (() => {
        const weakMap = new WeakMap();
        const key = {};
        weakMap.set(key, { key });

        // The first key's value is the second key, whose value refers back to the first.
        const first = {};
        const second = {};
        weakMap.set(first, second);
        weakMap.set(second, { first });
        return weakMap;
    })();
    expect(getWeakMapSize(weakMap)).toBe(3);

    gc();

    expect(getWeakMapSize(weakMap)).toBe(0);
});

test("a value stays alive while its key is reachable through another entry", () => {
    const weakMap = new WeakMap();
    const first = {};
    let second = {};
    weakMap.set(first, second);
    weakMap.set(second, { marker: 42 });
    (() => second)();

    markAsGarbage("second");
    gc();

    expect(getWeakMapSize(weakMap)).toBe(2);
    expect(weakMap.get(weakMap.get(first)).marker).toBe(42);
});
