test("length", () => {
    expect(Array.prototype.keys.length).toBe(0);
});

test("basic functionality", () => {
    const a = ["a", "b", "c"];
    const it = a.keys();
    expect(it.next()).toEqual({ value: 0, done: false });
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("works when applied to non-object", () => {
    [true, false, 9, 2n, Symbol()].forEach(primitive => {
        const it = [].keys.call(primitive);
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });
});

test("item added to array before exhaustion is accessible", () => {
    const a = ["a", "b"];
    const it = a.keys();
    expect(it.next()).toEqual({ value: 0, done: false });
    expect(it.next()).toEqual({ value: 1, done: false });
    a.push("c");
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("item added to array after exhaustion is inaccessible", () => {
    const a = ["a", "b"];
    const it = a.keys();
    expect(it.next()).toEqual({ value: 0, done: false });
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    a.push("c");
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].keys).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            keys;
        }).toThrowWithMessage(ReferenceError, "'keys' is not defined");
    }
});
