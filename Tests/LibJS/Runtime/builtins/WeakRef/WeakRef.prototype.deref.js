test("length is 0", () => {
    expect(WeakRef.prototype.deref).toHaveLength(0);
});

test("basic functionality", () => {
    var originalObject = { a: 1 };
    var objectWeakRef = new WeakRef(originalObject);

    expect(objectWeakRef.deref()).toBe(originalObject);

    var originalSymbol = { a: 1 };
    var symbolWeakRef = new WeakRef(originalSymbol);

    expect(symbolWeakRef.deref()).toBe(originalSymbol);
});

test("object kept alive for current synchronous execution sequence", () => {
    var weakRef;
    {
        weakRef = new WeakRef({ a: 1 });
    }
    weakRef.deref();
    gc();
    // This is fine 🔥
    expect(weakRef.deref()).not.toBe(undefined);
});

test("symbol kept alive for current synchronous execution sequence", () => {
    var weakRef;
    {
        weakRef = new WeakRef(Symbol("foo"));
    }
    weakRef.deref();
    gc();
    // This is fine 🔥
    expect(weakRef.deref()).not.toBe(undefined);
});
