test("length", () => {
    expect(Array.prototype[Symbol.iterator]).toHaveLength(0);
});

test("@@toStringTag", () => {
    expect([].values()[Symbol.toStringTag]).toBe("Array Iterator");
    expect([].values().toString()).toBe("[object Array Iterator]");
});

test("same function as Array.prototype.values", () => {
    expect(Array.prototype[Symbol.iterator]).toBe(Array.prototype.values);
});

test("basic functionality", () => {
    const a = [1, 2, 3];
    const it = a[Symbol.iterator]();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: 3, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("works when applied to non-object", () => {
    [true, false, 9, 2n, Symbol()].forEach(primitive => {
        const it = [][Symbol.iterator].call(primitive);
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });
});

test("item added to array before exhaustion is accessible", () => {
    const a = [1, 2];
    const it = a[Symbol.iterator]();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    a.push(3);
    expect(it.next()).toEqual({ value: 3, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("item added to array after exhaustion is inaccessible", () => {
    const a = [1, 2];
    const it = a[Symbol.iterator]();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    a.push(3);
    expect(it.next()).toEqual({ value: undefined, done: true });
});
