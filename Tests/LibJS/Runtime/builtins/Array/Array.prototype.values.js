test("length", () => {
    expect(Array.prototype.values.length).toBe(0);
});

test("basic functionality", () => {
    const a = [1, 2, 3];
    const it = a.values();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: 3, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("works when applied to non-object", () => {
    [true, false, 9, 2n, Symbol()].forEach(primitive => {
        const it = [].values.call(primitive);
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });
});

test("item added to array before exhaustion is accessible", () => {
    const a = [1, 2];
    const it = a.values();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    a.push(3);
    expect(it.next()).toEqual({ value: 3, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("item added to array after exhaustion is inaccessible", () => {
    const a = [1, 2];
    const it = a.values();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    a.push(3);
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].values).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            values;
        }).toThrowWithMessage(ReferenceError, "'values' is not defined");
    }
});
