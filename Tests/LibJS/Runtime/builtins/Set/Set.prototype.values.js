test("length", () => {
    expect(Set.prototype.values.length).toBe(0);
});

test("basic functionality", () => {
    const a = new Set([1, 2, 3]);
    const it = a.values();
    expect(it.next()).toEqual({ value: 1, done: false });
    expect(it.next()).toEqual({ value: 2, done: false });
    expect(it.next()).toEqual({ value: 3, done: false });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
    expect(it.next()).toEqual({ value: undefined, done: true });
});

describe("keys is an alias for values", () => {
    test("length", () => {
        expect(Set.prototype.keys.length).toBe(0);
    });

    test("basic functionality", () => {
        const a = new Set([1, 2, 3]);
        const it = a.keys();
        expect(it.next()).toEqual({ value: 1, done: false });
        expect(it.next()).toEqual({ value: 2, done: false });
        expect(it.next()).toEqual({ value: 3, done: false });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
        expect(it.next()).toEqual({ value: undefined, done: true });
    });
});
