test("length", () => {
    expect(Array.prototype.entries.length).toBe(0);
});

test("basic functionality", () => {
    const a = ["a", "b", "c"];
    const it = a.entries();
    expect(it.next()).toEqual({ value: [0, "a"], done: false });
    expect(it.next()).toEqual({ value: [1, "b"], done: false });
    expect(it.next()).toEqual({ value: [2, "c"], done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("works when applied to non-object", () => {
    [true, false, 9, 2n, Symbol()].forEach(primitive => {
        const it = [].entries.call(primitive);
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });
});

test("item added to array before exhaustion is accessible", () => {
    const a = ["a", "b"];
    const it = a.entries();
    expect(it.next()).toEqual({ value: [0, "a"], done: false });
    expect(it.next()).toEqual({ value: [1, "b"], done: false });
    a.push("c");
    expect(it.next()).toEqual({ value: [2, "c"], done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("item added to array after exhaustion is inaccessible", () => {
    const a = ["a", "b"];
    const it = a.entries();
    expect(it.next()).toEqual({ value: [0, "a"], done: false });
    expect(it.next()).toEqual({ value: [1, "b"], done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    a.push("c");
    expect(it.next()).toEqual({ value: undefined, done: true });
});

test("is unscopable", () => {
    expect(Array.prototype[Symbol.unscopables].entries).toBeTrue();
    const array = [];
    with (array) {
        expect(() => {
            entries;
        }).toThrowWithMessage(ReferenceError, "'entries' is not defined");
    }
});
